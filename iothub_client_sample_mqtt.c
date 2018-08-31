// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <stdio.h>
#include <stdlib.h>

#include "esp_system.h"
#include "esp_log.h"

#include "iothub_client.h"
#include "iothub_message.h"
#include "azure_c_shared_utility/threadapi.h"
#include "azure_c_shared_utility/crt_abstractions.h"
#include "azure_c_shared_utility/platform.h"
#include "iothubtransportmqtt.h"
#include "parson.h"

#ifdef MBED_BUILD_TIMESTAMP
#include "certs.h"
#endif // MBED_BUILD_TIMESTAMP

/*String containing Hostname, Device Id & Device Key in the format:                         */
/*  "HostName=<host_name>;DeviceId=<device_id>;SharedAccessKey=<device_key>"                */
/*  "HostName=<host_name>;DeviceId=<device_id>;SharedAccessSignature=<device_sas_token>"    */
#define EXAMPLE_IOTHUB_CONNECTION_STRING CONFIG_IOTHUB_CONNECTION_STRING
static const char* connectionString = EXAMPLE_IOTHUB_CONNECTION_STRING;

static const char *TAG = "iothub_client_sample_mqtt";

static int callbackCounter;
static char msgText[1024];
static char propText[1024];
static bool g_continueRunning;

typedef struct GEO_TAG
{
    double longitude;
    double latitude;
} Geo;

typedef struct SETTINGS_TAG
{
    char* localContact;
    int sleepDuration;
    Geo location;
} Settings;

static char* strDupe(const char* src)
{
    char* result = malloc(strlen(src) + 1);
    strcpy(result, src);
    return result;
}

static char* serializeToJson(Settings* settings)
{
    char* result;

    JSON_Value* root_value = json_value_init_object();
    JSON_Object* root_object = json_value_get_object(root_value);

    json_object_set_string(root_object, "localContact", settings->localContact);
    json_object_set_number(root_object, "sleepDuration", settings->sleepDuration);
    json_object_dotset_number(root_object, "location.longitude", settings->location.longitude);
    json_object_dotset_number(root_object, "location.latitude", settings->location.latitude);

    result = json_serialize_to_string(root_value);
    json_value_free(root_value);
    
    return result;
}

static Settings* parseFromJson(const char* json, DEVICE_TWIN_UPDATE_STATE update_state)
{
    Settings* result = malloc(sizeof(Settings));
    memset(result, 0, sizeof(Settings));

    JSON_Value* root_value = json_parse_string(json);
    JSON_Object* root_object = json_value_get_object(root_value);

    JSON_Value* localContact;
    JSON_Value* sleepDuration;
    JSON_Value* longitude;
    JSON_Value* latitude;

    if (update_state == DEVICE_TWIN_UPDATE_COMPLETE)
    {
        localContact = json_object_dotget_value(root_object, "desired.localContact");
        sleepDuration = json_object_dotget_value(root_object, "desired.sleepDuration");
        longitude = json_object_dotget_value(root_object, "desired.location.longitude");
        latitude = json_object_dotget_value(root_object, "desired.location.latitude");
    }
    else
    {
        localContact = json_object_get_value(root_object, "localContact");
        sleepDuration = json_object_get_value(root_object, "sleepDuration");
        longitude = json_object_dotget_value(root_object, "location.longitude");
        latitude = json_object_dotget_value(root_object, "location.latitude");
    }

    if (localContact != NULL)
    {
        const char* data = json_value_get_string(localContact);

        if (data != NULL)
        {
            result->localContact = strDupe(data);
        }
    }

    if (sleepDuration != NULL)
    {
        result->sleepDuration = (int)json_value_get_number(sleepDuration);
    }

    if (latitude != NULL)
    {
        result->location.latitude = json_value_get_number(latitude);
    }

    if (longitude != NULL)
    {
        result->location.longitude = json_value_get_number(longitude);
    }

    json_value_free(root_value);

    return result;
}

static void CHECK(IOTHUB_CLIENT_RESULT result, char* func)
{
    if (result != IOTHUB_CLIENT_OK)
    {
        ESP_LOGE(TAG, "%s failed (%s)", func, ENUM_TO_STRING(IOTHUB_CLIENT_RESULT, result));
    }
    else
    {
        ESP_LOGI(TAG, "%s succeeded", func);
    }
}

#define DOWORK_LOOP_NUM     10

#ifdef RECEIVER
static unsigned char* bytearray_to_str(const unsigned char *buffer, size_t len)
{
    unsigned char* ret = (unsigned char*)malloc(len+1);
    memcpy(ret, buffer, len);
    ret[len] = '\0';
    return ret;
}
#endif // RECEIVER

static void ConnectionStatusCallback(IOTHUB_CLIENT_CONNECTION_STATUS result, IOTHUB_CLIENT_CONNECTION_STATUS_REASON reason, void* userContextCallback)
{
    int* counter = (int*)userContextCallback;

    if (counter == NULL) {
        ESP_LOGI(TAG, "***** ----- ***** ConnectionStatus result=%s reason=%s", 
            ENUM_TO_STRING(IOTHUB_CLIENT_CONNECTION_STATUS, result), 
            ENUM_TO_STRING(IOTHUB_CLIENT_CONNECTION_STATUS_REASON, reason));
        if (result == IOTHUB_CLIENT_CONNECTION_UNAUTHENTICATED && reason == IOTHUB_CLIENT_CONNECTION_NO_NETWORK)
        {
            g_continueRunning = false; 
        }
    }
}

#ifdef RECEIVER
static IOTHUBMESSAGE_DISPOSITION_RESULT ReceiveMessageCallback(IOTHUB_MESSAGE_HANDLE message, void* userContextCallback)
{

    int* counter = (int*)userContextCallback;
    const char* buffer;
    size_t size;

    if (IoTHubMessage_GetByteArray(message, (const unsigned char**)&buffer, &size) != IOTHUB_MESSAGE_OK)
    {
        ESP_LOGI(TAG, "unable to retrieve the message data");
    }
    else
    {
        unsigned char* message_string = bytearray_to_str((const unsigned char *)buffer, size);
        ESP_LOGI(TAG, "IoTHubMessage_GetByteArray received message: \"%s\"", message_string);
        free(message_string);

        // If we receive the word 'quit' then we stop running
        if (size == (strlen("quit") * sizeof(char)) && memcmp(buffer, "quit", size) == 0)
        {
            g_continueRunning = false;
        }
    }

    // Retrieve properties from the message
    MAP_HANDLE mapProperties = IoTHubMessage_Properties(message);
    if (mapProperties != NULL)
    {
        const char*const* keys;
        const char*const* values;
        size_t propertyCount = 0;
        if (Map_GetInternals(mapProperties, &keys, &values, &propertyCount) == MAP_OK)
        {
            if (propertyCount > 0)
            {
                size_t index = 0;
                for (index = 0; index < propertyCount; index++)
                {
                    //ESP_LOGI(TAG, "\tKey: %s Value: %s", keys[index], values[index]);
                }
            }
        }
    }

    // Some device specific action code goes here...
    (*counter)++;
    return IOTHUBMESSAGE_ACCEPTED;
}
#endif

static void deviceTwinCallback(DEVICE_TWIN_UPDATE_STATE update_state, const unsigned char* payLoad, size_t size, void* userContextCallback)
{
    Settings* oldSettings = (Settings*)userContextCallback;
    Settings* newSettings = parseFromJson((const char*)payLoad, update_state);

    ESP_LOGI(TAG, "deviceTwinCallback received state %s", ENUM_TO_STRING(DEVICE_TWIN_UPDATE_STATE, update_state));

    if (newSettings->localContact != NULL)
    {
        if (oldSettings->localContact == NULL || strcmp(oldSettings->localContact, newSettings->localContact) != 0)
        {
            ESP_LOGI(TAG, "Received a new localContact = %s", newSettings->localContact);

            if (oldSettings->localContact != NULL)
            {
                free(oldSettings->localContact);
            }

            oldSettings->localContact = strDupe(newSettings->localContact);
        }
    }

    if (newSettings->sleepDuration != 0)
    {
        if (newSettings->sleepDuration != oldSettings->sleepDuration)
        {
            ESP_LOGI(TAG, "Received a new sleepDuration = %d", newSettings->sleepDuration);
            oldSettings->sleepDuration = newSettings->sleepDuration;
        }
    }

    if (newSettings->location.latitude != 0 || newSettings->location.longitude != 0)
    {
        if (newSettings->location.latitude != oldSettings->location.latitude || newSettings->location.longitude != oldSettings->location.longitude)
        {
            ESP_LOGI(TAG, "Received a new lat/long = %f,%f", newSettings->location.latitude, newSettings->location.longitude);
            oldSettings->location.latitude = newSettings->location.latitude;
            oldSettings->location.longitude = newSettings->location.longitude;
        }
    }

    free(newSettings);
}

static void reportedStateCallback(int status_code, void* userContextCallback)
{
    (void)userContextCallback;

    ESP_LOGI(TAG, "Device Twin reported properties update completed with result: %d", status_code);
}

static void SendConfirmationCallback(IOTHUB_CLIENT_CONFIRMATION_RESULT result, void* userContextCallback)
{
    int iterator = (int)userContextCallback;

    ESP_LOGI(TAG, "Confirmation[%d] received for message [%d] with result = %s", callbackCounter, iterator, ENUM_TO_STRING(IOTHUB_CLIENT_CONFIRMATION_RESULT, result));
    callbackCounter++;
}

void DoWork(IOTHUB_CLIENT_LL_HANDLE iotHubClientHandle)
{
    size_t index = 0;
    for (index = 0; index < DOWORK_LOOP_NUM; index++)
    {
        IoTHubClient_LL_DoWork(iotHubClientHandle);
        ThreadAPI_Sleep(100);
    }
}

void iothub_client_sample_mqtt_run(void)
{
	ESP_LOGI(TAG, "File:%s Compile Time:%s %s",__FILE__,__DATE__,__TIME__);
    IOTHUB_CLIENT_LL_HANDLE iotHubClientHandle = NULL;

    g_continueRunning = true;
    srand((unsigned int)time(NULL));
    double avgWindSpeed = 10.0;

    callbackCounter = 0;

    IOTHUB_CLIENT_RESULT result = (platform_init() == 0) ? IOTHUB_CLIENT_OK : IOTHUB_CLIENT_ERROR;
    CHECK(result, "platform_init");

    if (result == IOTHUB_CLIENT_OK)
    {
        iotHubClientHandle = IoTHubClient_LL_CreateFromConnectionString(connectionString, MQTT_Protocol);
        result = (iotHubClientHandle != NULL) ? IOTHUB_CLIENT_OK : IOTHUB_CLIENT_ERROR;
        CHECK(result, "IoTHubClient_LL_CreateFromConnectionString");
    }

    bool traceOn = true;
    if (result == IOTHUB_CLIENT_OK)
    {
        result = IoTHubClient_LL_SetOption(iotHubClientHandle, "logtrace", &traceOn);
        CHECK(result, "IoTHubClient_LL_SetOption(logtrace)");
    }

#ifdef MBED_BUILD_TIMESTAMP
    if (result == IOTHUB_CLIENT_OK)
    {
        resutl = IoTHubClient_LL_SetOption(iotHubClientHandle, "TrustedCerts", certificates);
        CHECK(result, "IoTHubClient_LL_SetOption(TrustedCerts)");
    }
#endif // MBED_BUILD_TIMESTAMP


    if (result == IOTHUB_CLIENT_OK)
    {
        result = IoTHubClient_LL_SetConnectionStatusCallback(iotHubClientHandle, ConnectionStatusCallback, NULL);
        CHECK(result, "IoTHubClient_LL_SetConnectionStatusCallback");
    }

    /* Setting Message call back, so we can receive Commands. */
#ifdef RECEIVER
    int receiveContext = 0;
    if (result == IOTHUB_CLIENT_OK)
    {
        result = IoTHubClient_LL_SetMessageCallback(iotHubClientHandle, ReceiveMessageCallback, &receiveContext);
        CHECK(result, "IoTHubClient_LL_SetMessageCallback");
    }
#endif // RECEIVER

    Settings settings;
    ESP_LOGI(TAG, "zeroing settings");
    memset(&settings, 0, sizeof(Settings));


    ESP_LOGI(TAG, "setting settings.localContact");
    settings.localContact = strDupe("SEANK");
    ESP_LOGI(TAG, "setting settings.sleepDuration");
    settings.sleepDuration = 5000;
    ESP_LOGI(TAG, "setting settings.location.latitude");
    settings.location.latitude = 40.6892;
    ESP_LOGI(TAG, "setting settings.location.longitude");
    settings.location.longitude = 74.0445;

    if (result == IOTHUB_CLIENT_OK)
    {
        result = IoTHubClient_LL_SetDeviceTwinCallback(iotHubClientHandle, deviceTwinCallback, &settings);
        CHECK(result, "IoTHubDeviceClient_SetDeviceTwinCallback");
    }

    if (result == IOTHUB_CLIENT_OK)
    {
        char* reportedProperties = serializeToJson(&settings);
        result = IoTHubClient_LL_SendReportedState(iotHubClientHandle, (const unsigned char*)reportedProperties, strlen(reportedProperties), reportedStateCallback, NULL);
        CHECK(result, "IoTHubDeviceClient_SendReportedState");
        free(reportedProperties);
    }

    /* Now that we are ready to receive commands, let's send some messages */
    int iterator = 0;
    if (result == IOTHUB_CLIENT_OK)
    {
        do
        {
            sprintf_s(msgText, sizeof(msgText), "{\"deviceId\":\"AirConditionDevice_001\",\"windSpeed\":%.2f}", avgWindSpeed + (rand() % 4 + 2));
            ESP_LOGI(TAG, "Ready to Send String:%s as message %d",(const char*)msgText, iterator);

            IOTHUB_MESSAGE_HANDLE messageHandle = IoTHubMessage_CreateFromByteArray((const unsigned char*)msgText, strlen(msgText));
            result = (messageHandle != NULL) ? IOTHUB_CLIENT_OK : IOTHUB_CLIENT_ERROR;
            CHECK(result, "IoTHubMessage_CreateFromByteArray");

            if (result == IOTHUB_CLIENT_OK)
            {
                MAP_HANDLE propMap = IoTHubMessage_Properties(messageHandle);
                (void)sprintf_s(propText, sizeof(propText), "PropMsg_%zu", iterator);
                result = (Map_AddOrUpdate(propMap, "PropName", propText) == MAP_OK) ? IOTHUB_CLIENT_OK : IOTHUB_CLIENT_ERROR;
                CHECK(result, "Map_AddOrUpdate");
            }

            if (result == IOTHUB_CLIENT_OK)
            {
                result = IoTHubClient_LL_SendEventAsync(iotHubClientHandle, messageHandle, SendConfirmationCallback, (void*)iterator);
                CHECK(result, "IoTHubClient_LL_SendEventAsync");
            }

            if (messageHandle != NULL)
            {
                IoTHubMessage_Destroy(messageHandle);
            }
            iterator++;

            DoWork(iotHubClientHandle);

            ESP_LOGI(TAG, "Sleeping for 5");
            ThreadAPI_Sleep(5000);
        } while (g_continueRunning);

        ESP_LOGI(TAG, "Connection lost...reseting...");
        DoWork(iotHubClientHandle);
    }

    if (iotHubClientHandle != NULL)
    {
        IoTHubClient_LL_Destroy(iotHubClientHandle);
    }

    free(settings.localContact);

    platform_deinit();
}

int main(void)
{
    iothub_client_sample_mqtt_run();
    return 0;
}
