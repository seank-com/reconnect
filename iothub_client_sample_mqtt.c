// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <stdio.h>
#include <stdlib.h>

#include "esp_system.h"

#include "iothub_client.h"
#include "iothub_message.h"
#include "azure_c_shared_utility/threadapi.h"
#include "azure_c_shared_utility/crt_abstractions.h"
#include "azure_c_shared_utility/platform.h"
#include "iothubtransportmqtt.h"

#ifdef MBED_BUILD_TIMESTAMP
#include "certs.h"
#endif // MBED_BUILD_TIMESTAMP

/*String containing Hostname, Device Id & Device Key in the format:                         */
/*  "HostName=<host_name>;DeviceId=<device_id>;SharedAccessKey=<device_key>"                */
/*  "HostName=<host_name>;DeviceId=<device_id>;SharedAccessSignature=<device_sas_token>"    */
#define EXAMPLE_IOTHUB_CONNECTION_STRING CONFIG_IOTHUB_CONNECTION_STRING
static const char* connectionString = EXAMPLE_IOTHUB_CONNECTION_STRING;

static int callbackCounter;
static char msgText[1024];
static char propText[1024];
static bool g_continueRunning;

#define DOWORK_LOOP_NUM     10

#ifdef RECEIVER
static unsigned char* bytearray_to_str(const unsigned char *buffer, size_t len)
{
    unsigned char* ret = (unsigned char*)malloc(len+1);
    memcpy(ret, buffer, len);
    ret[len] = '\0';
    return ret;
}
#endif

static void ConnectionStatusCallback(IOTHUB_CLIENT_CONNECTION_STATUS result, IOTHUB_CLIENT_CONNECTION_STATUS_REASON reason, void* userContextCallback)
{
    int* counter = (int*)userContextCallback;

    if (counter == NULL) {
        (void)printf("***** ----- ***** ConnectionStatus result=%s reason=%s\r\n", 
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
        (void)printf("unable to retrieve the message data\r\n");
    }
    else
    {
        unsigned char* message_string = bytearray_to_str((const unsigned char *)buffer, size);
        (void)printf("IoTHubMessage_GetByteArray received message: \"%s\" \n", message_string);
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
                    //(void)printf("\tKey: %s Value: %s\r\n", keys[index], values[index]);
                }
                //(void)printf("\r\n");
            }
        }
    }

    // Some device specific action code goes here...
    (*counter)++;
    return IOTHUBMESSAGE_ACCEPTED;
}
#endif

static void SendConfirmationCallback(IOTHUB_CLIENT_CONFIRMATION_RESULT result, void* userContextCallback)
{
    IOTHUB_MESSAGE_HANDLE messageHandle = (IOTHUB_MESSAGE_HANDLE)userContextCallback;

    (void)printf("Confirmation[%d] received for message with result = %s\r\n", callbackCounter, ENUM_TO_STRING(IOTHUB_CLIENT_CONFIRMATION_RESULT, result));
    /* Some device specific action code goes here... */
    callbackCounter++;
    IoTHubMessage_Destroy(messageHandle);
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
	printf("\nFile:%s Compile Time:%s %s\n",__FILE__,__DATE__,__TIME__);
    IOTHUB_CLIENT_LL_HANDLE iotHubClientHandle;

    g_continueRunning = true;
    srand((unsigned int)time(NULL));
    double avgWindSpeed = 10.0;

    callbackCounter = 0;

#ifdef RECEIVER
    int receiveContext = 0;
#endif

    if (platform_init() != 0)
    {
        (void)printf("Failed to initialize the platform.\r\n");
    }
    else
    {
        if ((iotHubClientHandle = IoTHubClient_LL_CreateFromConnectionString(connectionString, MQTT_Protocol)) == NULL)
        {
            (void)printf("ERROR: iotHubClientHandle is NULL!\r\n");
        }
        else
        {
            bool traceOn = true;
            IoTHubClient_LL_SetOption(iotHubClientHandle, "logtrace", &traceOn);

#ifdef MBED_BUILD_TIMESTAMP
            // For mbed add the certificate information
            if (IoTHubClient_LL_SetOption(iotHubClientHandle, "TrustedCerts", certificates) != IOTHUB_CLIENT_OK)
            {
                printf("failure to set option \"TrustedCerts\"\r\n");
            }
#endif // MBED_BUILD_TIMESTAMP

            if (IoTHubClient_LL_SetConnectionStatusCallback(iotHubClientHandle, ConnectionStatusCallback, NULL) != IOTHUB_CLIENT_OK)
            {
                (void)printf("ERROR: IoTHubClient_LL_SetConnectionStatusCallback..........FAILED!\r\n");
            }

            /* Setting Message call back, so we can receive Commands. */
#ifdef RECEIVER            
            if (IoTHubClient_LL_SetMessageCallback(iotHubClientHandle, ReceiveMessageCallback, &receiveContext) != IOTHUB_CLIENT_OK)
            {
                (void)printf("ERROR: IoTHubClient_LL_SetMessageCallback..........FAILED!\r\n");
            }
            else
            {
                (void)printf("IoTHubClient_LL_SetMessageCallback...successful.\r\n");
#endif
                /* Now that we are ready to receive commands, let's send some messages */
                size_t iterator = 0;

                do
                {
                    IOTHUB_MESSAGE_HANDLE messageHandle;

                    sprintf_s(msgText, sizeof(msgText), "{\"deviceId\":\"AirConditionDevice_001\",\"windSpeed\":%.2f}", avgWindSpeed + (rand() % 4 + 2));
                    printf("Ready to Send String:%s\n",(const char*)msgText);
                    if ((messageHandle = IoTHubMessage_CreateFromByteArray((const unsigned char*)msgText, strlen(msgText))) == NULL)
                    {
                        (void)printf("ERROR: iotHubMessageHandle is NULL!\r\n");
                    }
                    else
                    {
                        MAP_HANDLE propMap = IoTHubMessage_Properties(messageHandle);
                        (void)sprintf_s(propText, sizeof(propText), "PropMsg_%zu", iterator);
                        if (Map_AddOrUpdate(propMap, "PropName", propText) != MAP_OK)
                        {
                            (void)printf("ERROR: Map_AddOrUpdate Failed!\r\n");
                        }
                        if (IoTHubClient_LL_SendEventAsync(iotHubClientHandle, messageHandle, SendConfirmationCallback, messageHandle) != IOTHUB_CLIENT_OK)
                        {
                            (void)printf("ERROR: IoTHubClient_LL_SendEventAsync..........FAILED!\r\n");
                        }
                        else
                        {
                            (void)printf("IoTHubClient_LL_SendEventAsync accepted message [%d] for transmission to IoT Hub.\r\n", (int)iterator);
                        }
                    }
                    iterator++;

                    DoWork(iotHubClientHandle);

                    printf("Sleeping for 5\n");
                    ThreadAPI_Sleep(5000);

                } while (g_continueRunning);

                (void)printf("Connection lost...reseting...\r\n");
                DoWork(iotHubClientHandle);
#ifdef RECEIVER            
            }
#endif
            IoTHubClient_LL_Destroy(iotHubClientHandle);
        }
        platform_deinit();
    }
}

int main(void)
{
    iothub_client_sample_mqtt_run();
    return 0;
}
