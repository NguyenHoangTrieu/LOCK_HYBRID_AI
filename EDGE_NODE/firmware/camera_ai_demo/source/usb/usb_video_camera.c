/*
 * usb_video_camera.c - see usb_video_camera.h. Abandoned path, see
 * WORKLOG.md.
 *
 * Adapted from mcuxsdk's usb_device_video_virtual_camera example.
 * Differences: the payload-fill function reads real pixels straight from
 * the live camera buffer (RGB565) and converts to YUY2 on the fly instead
 * of scanning a static MJPEG array; still-image capture is dropped (not
 * advertised in the descriptor); only one frame interval (30fps, the
 * camera's fixed rate) is offered, so there's nothing to throttle.
 */
#include <stdio.h>
#include <stdlib.h>

#include "usb_device_config.h"
#include "usb.h"
#include "usb_device.h"

#include "usb_device_class.h"
#include "usb_device_video.h"

#include "usb_device_ch9.h"
#include "usb_device_descriptor.h"

#include "usb_video_camera.h"
#include "camera_capture.h"
#include "app.h"

#include "fsl_device_registers.h"
#include "clock_config.h"
#include "fsl_debug_console.h"
#include "board.h"

#if (defined(FSL_FEATURE_SOC_SYSMPU_COUNT) && (FSL_FEATURE_SOC_SYSMPU_COUNT > 0U))
#include "fsl_sysmpu.h"
#endif /* FSL_FEATURE_SOC_SYSMPU_COUNT */

/*******************************************************************************
 * Prototypes
 ******************************************************************************/
void BOARD_InitHardware(void);
void USB_DeviceClockInit(void);
void USB_DeviceIsrEnable(void);
#if USB_DEVICE_CONFIG_USE_TASK
void USB_DeviceTaskFn(void *deviceHandle);
#endif

static void USB_DeviceVideoPrepareVideoData(void);
static usb_status_t USB_DeviceVideoRequest(class_handle_t handle, uint32_t event, void *param);
static usb_status_t USB_DeviceVideoCallback(class_handle_t handle, uint32_t event, void *param);
static void USB_DeviceVideoApplicationSetDefault(void);
static usb_status_t USB_DeviceCallback(usb_device_handle handle, uint32_t event, void *param);

/*******************************************************************************
 * Variables
 ******************************************************************************/

extern usb_device_class_struct_t g_UsbDeviceVideoVirtualCameraConfig;

USB_DMA_NONINIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static usb_device_video_probe_and_commit_controls_struct_t s_ProbeStruct;
USB_DMA_NONINIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static usb_device_video_probe_and_commit_controls_struct_t s_CommitStruct;
USB_DMA_NONINIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static uint32_t s_ClassRequestBuffer[(sizeof(usb_device_video_probe_and_commit_controls_struct_t) >> 2U) + 1U];
/* Scratch buffer for one USB IN packet: 2-byte UVC payload header + up to (HS_STREAM_IN_PACKET_SIZE - 2) bytes of
 * converted YUY2 pixel data. The *source* pixels stay in the camera's own frame buffer - this only ever holds one
 * packet's worth of already-converted output, not a whole extra frame. */
USB_DMA_NONINIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE) static uint8_t s_ImageBuffer[HS_STREAM_IN_PACKET_SIZE];
usb_video_virtual_camera_struct_t g_UsbDeviceVideoVirtualCamera;

usb_device_class_config_struct_t g_UsbDeviceVideoConfig[1] = {{
    USB_DeviceVideoCallback,
    (class_handle_t)NULL,
    &g_UsbDeviceVideoVirtualCameraConfig,
}};

usb_device_class_config_list_struct_t g_UsbDeviceVideoConfigList = {
    g_UsbDeviceVideoConfig,
    USB_DeviceCallback,
    1U,
};

/*******************************************************************************
 * Code
 ******************************************************************************/

static uint8_t USB_VideoCamera_ClampToU8(int32_t value)
{
    if (value < 0)
    {
        return 0U;
    }
    if (value > 255)
    {
        return 255U;
    }
    return (uint8_t)value;
}

/* BT.601 RGB->YCbCr, standard integer fixed-point coefficients (same ones used by most software converters). */
static void USB_VideoCamera_Rgb565ToYCbCr(uint16_t pixel, uint8_t *y, uint8_t *cb, uint8_t *cr)
{
    uint32_t r5 = (uint32_t)(pixel >> 11) & 0x1FU;
    uint32_t g6 = (uint32_t)(pixel >> 5) & 0x3FU;
    uint32_t b5 = (uint32_t)pixel & 0x1FU;
    /* 5/6-bit -> 8-bit expansion (replicate the high bits into the low bits, the standard cheap approximation). */
    int32_t r8 = (int32_t)((r5 << 3) | (r5 >> 2));
    int32_t g8 = (int32_t)((g6 << 2) | (g6 >> 4));
    int32_t b8 = (int32_t)((b5 << 3) | (b5 >> 2));

    *y  = USB_VideoCamera_ClampToU8(((66 * r8 + 129 * g8 + 25 * b8 + 128) / 256) + 16);
    *cb = USB_VideoCamera_ClampToU8(((-38 * r8 - 74 * g8 + 112 * b8 + 128) / 256) + 128);
    *cr = USB_VideoCamera_ClampToU8(((112 * r8 - 94 * g8 - 18 * b8 + 128) / 256) + 128);
}

/* Converts up to `pixelCount` source pixels (must be even) starting at `firstPixel` of the live camera frame into
 * YUY2 macropixels written to `out`. Returns the number of bytes written (4 bytes per 2 source pixels). */
static uint32_t USB_VideoCamera_ConvertFrameChunk(uint32_t firstPixel, uint32_t pixelCount, uint8_t *out)
{
    const uint16_t *frame = CAMERA_CAPTURE_GetFrameBuffer();
    uint32_t bytesWritten = 0U;

    for (uint32_t i = 0U; i < pixelCount; i += 2U)
    {
        uint8_t y0, y1, cb0, cr0, cb1, cr1;
        USB_VideoCamera_Rgb565ToYCbCr(frame[firstPixel + i], &y0, &cb0, &cr0);
        USB_VideoCamera_Rgb565ToYCbCr(frame[firstPixel + i + 1U], &y1, &cb1, &cr1);

        /* YUY2 byte order: Y0 U Y1 V, chroma shared between the pixel pair (average the two samples). */
        out[bytesWritten + 0U] = y0;
        out[bytesWritten + 1U] = (uint8_t)(((uint32_t)cb0 + cb1) / 2U);
        out[bytesWritten + 2U] = y1;
        out[bytesWritten + 3U] = (uint8_t)(((uint32_t)cr0 + cr1) / 2U);
        bytesWritten += 4U;
    }

    return bytesWritten;
}

/* Prepare next transfer payload */
static void USB_DeviceVideoPrepareVideoData(void)
{
    usb_device_video_mjpeg_payload_header_struct_t *payloadHeader;
    const uint32_t totalPixels = (uint32_t)DEMO_BUFFER_WIDTH * DEMO_BUFFER_HEIGHT;
    uint32_t maxPacketSize;
    uint32_t payloadCapacity;
    uint32_t pixelsRemaining;
    uint32_t pixelsThisPacket;
    uint32_t bytesWritten;

    /* payload header lives at the front of imageBuffer */
    payloadHeader = (usb_device_video_mjpeg_payload_header_struct_t *)&g_UsbDeviceVideoVirtualCamera.imageBuffer[0];
    payloadHeader->bHeaderLength                                  = 2U; /* bHeaderLength + bmHeaderInfo only - no PTS/SCR fields sent */
    payloadHeader->headerInfoUnion.bmheaderInfo                   = 0U;
    payloadHeader->headerInfoUnion.headerInfoBits.frameIdentifier = g_UsbDeviceVideoVirtualCamera.currentFrameId;

    maxPacketSize   = USB_LONG_FROM_LITTLE_ENDIAN_DATA(g_UsbDeviceVideoVirtualCamera.commitStruct->dwMaxPayloadTransferSize);
    payloadCapacity = maxPacketSize - 2U; /* minus the 2-byte header this device always sends */
    payloadCapacity -= payloadCapacity % 4U; /* keep it a whole number of YUY2 macropixels (2 source pixels each) */

    pixelsRemaining  = totalPixels - g_UsbDeviceVideoVirtualCamera.framePixelOffset;
    pixelsThisPacket = (payloadCapacity / 4U) * 2U;
    if (pixelsThisPacket > pixelsRemaining)
    {
        pixelsThisPacket = pixelsRemaining;
    }

    bytesWritten = USB_VideoCamera_ConvertFrameChunk(g_UsbDeviceVideoVirtualCamera.framePixelOffset, pixelsThisPacket,
                                                      &g_UsbDeviceVideoVirtualCamera.imageBuffer[2]);
    g_UsbDeviceVideoVirtualCamera.imageBufferLength = 2U + bytesWritten;
    g_UsbDeviceVideoVirtualCamera.framePixelOffset += pixelsThisPacket;

    if (g_UsbDeviceVideoVirtualCamera.framePixelOffset >= totalPixels)
    {
        /* Finished streaming this frame - mark end-of-frame, toggle the frame ID, and start over
         * from pixel 0. CAMERA_CAPTURE_GetFrameBuffer() always points at the camera's single live
         * capture buffer (SmartDMA keeps overwriting it at 30fps), so the next packet naturally
         * pulls from the most recent capture - same tearing tradeoff the LCD path accepts. */
        payloadHeader->headerInfoUnion.headerInfoBits.endOfFrame = 1U;
        g_UsbDeviceVideoVirtualCamera.framePixelOffset           = 0U;
        g_UsbDeviceVideoVirtualCamera.currentFrameId ^= 1U;
    }
}

static usb_status_t USB_DeviceVideoRequest(class_handle_t handle, uint32_t event, void *param)
{
    usb_device_control_request_struct_t *request = (usb_device_control_request_struct_t *)param;
    usb_device_video_probe_and_commit_controls_struct_t *probe =
        (usb_device_video_probe_and_commit_controls_struct_t *)(request->buffer);
    usb_device_video_probe_and_commit_controls_struct_t *commit =
        (usb_device_video_probe_and_commit_controls_struct_t *)(request->buffer);
    uint32_t temp32;
    usb_status_t error = kStatus_USB_Success;

    switch (event)
    {
        /* probe request */
        case USB_DEVICE_VIDEO_SET_CUR_VS_PROBE_CONTROL:
            if ((request->buffer == NULL) || (request->length == 0U))
            {
                return kStatus_USB_InvalidRequest;
            }
            temp32 = USB_LONG_FROM_LITTLE_ENDIAN_DATA(probe->dwFrameInterval);
            if ((temp32 >= USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_MIN_INTERVAL) &&
                (temp32 <= USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_MAX_INTERVAL))
            {
                USB_LONG_TO_LITTLE_ENDIAN_DATA(temp32, g_UsbDeviceVideoVirtualCamera.probeStruct->dwFrameInterval);
            }
            temp32 = USB_LONG_FROM_LITTLE_ENDIAN_DATA(probe->dwMaxPayloadTransferSize);
            if ((temp32) && (temp32 < g_UsbDeviceVideoVirtualCamera.currentMaxPacketSize))
            {
                USB_LONG_TO_LITTLE_ENDIAN_DATA(temp32,
                                               g_UsbDeviceVideoVirtualCamera.probeStruct->dwMaxPayloadTransferSize);
            }
            g_UsbDeviceVideoVirtualCamera.probeStruct->bFormatIndex = probe->bFormatIndex;
            g_UsbDeviceVideoVirtualCamera.probeStruct->bFrameIndex  = probe->bFrameIndex;
            break;
        case USB_DEVICE_VIDEO_GET_CUR_VS_PROBE_CONTROL:
        /* GET_MIN/GET_MAX/GET_RES/GET_DEF fall through to the same answer as GET_CUR: this device
         * only supports one configuration (320x240 YUY2 @ 30fps), so min == max == res == def == cur.
         * Without these, Linux's uvcvideo STALLs and fails format negotiation - see WORKLOG.md. */
        case USB_DEVICE_VIDEO_GET_MIN_VS_PROBE_CONTROL:
        case USB_DEVICE_VIDEO_GET_MAX_VS_PROBE_CONTROL:
        case USB_DEVICE_VIDEO_GET_RES_VS_PROBE_CONTROL:
        case USB_DEVICE_VIDEO_GET_DEF_VS_PROBE_CONTROL:
            request->buffer = (uint8_t *)g_UsbDeviceVideoVirtualCamera.probeStruct;
            request->length = g_UsbDeviceVideoVirtualCamera.probeLength;
            break;
        case USB_DEVICE_VIDEO_GET_LEN_VS_PROBE_CONTROL:
            request->buffer = &g_UsbDeviceVideoVirtualCamera.probeLength;
            request->length = sizeof(g_UsbDeviceVideoVirtualCamera.probeLength);
            break;
        case USB_DEVICE_VIDEO_GET_INFO_VS_PROBE_CONTROL:
            request->buffer = &g_UsbDeviceVideoVirtualCamera.probeInfo;
            request->length = sizeof(g_UsbDeviceVideoVirtualCamera.probeInfo);
            break;
        /* commit request */
        case USB_DEVICE_VIDEO_SET_CUR_VS_COMMIT_CONTROL:
            if ((request->buffer == NULL) || (request->length == 0U))
            {
                return kStatus_USB_InvalidRequest;
            }
            temp32 = USB_LONG_FROM_LITTLE_ENDIAN_DATA(commit->dwFrameInterval);
            if ((temp32 >= USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_MIN_INTERVAL) &&
                (temp32 <= USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_MAX_INTERVAL))
            {
                USB_LONG_TO_LITTLE_ENDIAN_DATA(temp32, g_UsbDeviceVideoVirtualCamera.commitStruct->dwFrameInterval);
            }

            temp32 = USB_LONG_FROM_LITTLE_ENDIAN_DATA(commit->dwMaxPayloadTransferSize);
            if ((temp32) && (temp32 < g_UsbDeviceVideoVirtualCamera.currentMaxPacketSize))
            {
                USB_LONG_TO_LITTLE_ENDIAN_DATA(temp32,
                                               g_UsbDeviceVideoVirtualCamera.commitStruct->dwMaxPayloadTransferSize);
            }
            g_UsbDeviceVideoVirtualCamera.commitStruct->bFormatIndex = commit->bFormatIndex;
            g_UsbDeviceVideoVirtualCamera.commitStruct->bFrameIndex  = commit->bFrameIndex;
            break;
        case USB_DEVICE_VIDEO_GET_CUR_VS_COMMIT_CONTROL:
            request->buffer = (uint8_t *)g_UsbDeviceVideoVirtualCamera.commitStruct;
            request->length = g_UsbDeviceVideoVirtualCamera.commitLength;
            break;
        case USB_DEVICE_VIDEO_GET_LEN_VS_COMMIT_CONTROL:
            request->buffer = &g_UsbDeviceVideoVirtualCamera.commitLength;
            request->length = sizeof(g_UsbDeviceVideoVirtualCamera.commitLength);
            break;
        case USB_DEVICE_VIDEO_GET_INFO_VS_COMMIT_CONTROL:
            request->buffer = &g_UsbDeviceVideoVirtualCamera.commitInfo;
            request->length = sizeof(g_UsbDeviceVideoVirtualCamera.commitInfo);
            break;
        default:
            /* No still-image controls: this device's descriptor advertises bStillCaptureMethod = 0. */
            error = kStatus_USB_InvalidRequest;
            break;
    }
    return error;
}

/* USB device Video class callback */
static usb_status_t USB_DeviceVideoCallback(class_handle_t handle, uint32_t event, void *param)
{
    usb_status_t error = kStatus_USB_InvalidRequest;

    switch (event)
    {
        case kUSB_DeviceVideoEventStreamSendResponse:
            /* Stream data sent */
            if (g_UsbDeviceVideoVirtualCamera.attach)
            {
                /* Prepare the next stream data */
                USB_DeviceVideoPrepareVideoData();
                error = USB_DeviceVideoSend(
                    g_UsbDeviceVideoVirtualCamera.videoHandle, USB_VIDEO_VIRTUAL_CAMERA_STREAM_ENDPOINT_IN,
                    g_UsbDeviceVideoVirtualCamera.imageBuffer, g_UsbDeviceVideoVirtualCamera.imageBufferLength);
            }
            break;
        case kUSB_DeviceVideoEventClassRequestBuffer:
            if (param && (g_UsbDeviceVideoVirtualCamera.attach))
            {
                /* Get the class-specific OUT buffer */
                usb_device_control_request_struct_t *request = (usb_device_control_request_struct_t *)param;

                if (request->length <= sizeof(usb_device_video_probe_and_commit_controls_struct_t))
                {
                    request->buffer = (uint8_t *)g_UsbDeviceVideoVirtualCamera.classRequestBuffer;
                    error           = kStatus_USB_Success;
                }
            }
            break;
        default:
            if (param && (event > 0xFFU))
            {
                /* If the event is the class-specific request(Event > 0xFFU), handle the class-specific request */
                error = USB_DeviceVideoRequest(handle, event, param);
            }
            break;
    }

    return error;
}

/* Set to default state */
static void USB_DeviceVideoApplicationSetDefault(void)
{
    g_UsbDeviceVideoVirtualCamera.speed                = USB_SPEED_FULL;
    g_UsbDeviceVideoVirtualCamera.attach               = 0U;
    g_UsbDeviceVideoVirtualCamera.currentMaxPacketSize = HS_STREAM_IN_PACKET_SIZE;
    g_UsbDeviceVideoVirtualCamera.imageBuffer          = s_ImageBuffer;
    g_UsbDeviceVideoVirtualCamera.probeStruct          = &s_ProbeStruct;
    g_UsbDeviceVideoVirtualCamera.commitStruct         = &s_CommitStruct;
    g_UsbDeviceVideoVirtualCamera.classRequestBuffer   = &s_ClassRequestBuffer[0];
    for (uint8_t i = 0; i < USB_VIDEO_VIRTUAL_CAMERA_INTERFACE_COUNT; i++)
    {
        g_UsbDeviceVideoVirtualCamera.currentInterfaceAlternateSetting[i] = 0;
    }

    g_UsbDeviceVideoVirtualCamera.probeStruct->bFormatIndex = USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FORMAT_INDEX;
    g_UsbDeviceVideoVirtualCamera.probeStruct->bFrameIndex  = USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_INDEX;
    USB_LONG_TO_LITTLE_ENDIAN_DATA(USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_DEFAULT_INTERVAL,
                                   g_UsbDeviceVideoVirtualCamera.probeStruct->dwFrameInterval);
    USB_LONG_TO_LITTLE_ENDIAN_DATA(g_UsbDeviceVideoVirtualCamera.currentMaxPacketSize,
                                   g_UsbDeviceVideoVirtualCamera.probeStruct->dwMaxPayloadTransferSize);
    USB_LONG_TO_LITTLE_ENDIAN_DATA(USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_MAX_FRAME_SIZE,
                                   g_UsbDeviceVideoVirtualCamera.probeStruct->dwMaxVideoFrameSize);

    g_UsbDeviceVideoVirtualCamera.commitStruct->bFormatIndex = USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FORMAT_INDEX;
    g_UsbDeviceVideoVirtualCamera.commitStruct->bFrameIndex  = USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_INDEX;
    USB_LONG_TO_LITTLE_ENDIAN_DATA(USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_DEFAULT_INTERVAL,
                                   g_UsbDeviceVideoVirtualCamera.commitStruct->dwFrameInterval);
    USB_LONG_TO_LITTLE_ENDIAN_DATA(g_UsbDeviceVideoVirtualCamera.currentMaxPacketSize,
                                   g_UsbDeviceVideoVirtualCamera.commitStruct->dwMaxPayloadTransferSize);
    USB_LONG_TO_LITTLE_ENDIAN_DATA(USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_MAX_FRAME_SIZE,
                                   g_UsbDeviceVideoVirtualCamera.commitStruct->dwMaxVideoFrameSize);

    g_UsbDeviceVideoVirtualCamera.probeInfo    = 0x03U;
    g_UsbDeviceVideoVirtualCamera.probeLength  = 26U;
    g_UsbDeviceVideoVirtualCamera.commitInfo   = 0x03U;
    g_UsbDeviceVideoVirtualCamera.commitLength = 26U;

    g_UsbDeviceVideoVirtualCamera.currentFrameId                         = 0U;
    g_UsbDeviceVideoVirtualCamera.currentStreamInterfaceAlternateSetting = 0U;
    g_UsbDeviceVideoVirtualCamera.imageBufferLength                      = 0U;
    g_UsbDeviceVideoVirtualCamera.framePixelOffset                       = 0U;
}

/* The device callback */
static usb_status_t USB_DeviceCallback(usb_device_handle handle, uint32_t event, void *param)
{
    usb_status_t error = kStatus_USB_InvalidRequest;
    uint8_t *temp8     = (uint8_t *)param;
    uint16_t *temp16   = (uint16_t *)param;

    switch (event)
    {
        case kUSB_DeviceEventBusReset:
        {
            /* The device BUS reset signal detected */
            USB_DeviceVideoApplicationSetDefault();
            g_UsbDeviceVideoVirtualCamera.attach               = 0U;
            g_UsbDeviceVideoVirtualCamera.currentConfiguration = 0U;

#if (defined(USB_DEVICE_CONFIG_EHCI) && (USB_DEVICE_CONFIG_EHCI > 0U))
            /* Get USB speed to configure the device, including max packet size and interval of the endpoints. */
            if (kStatus_USB_Success == USB_DeviceGetStatus(g_UsbDeviceVideoVirtualCamera.deviceHandle,
                                                           kUSB_DeviceStatusSpeed,
                                                           &g_UsbDeviceVideoVirtualCamera.speed))
            {
                USB_DeviceSetSpeed(g_UsbDeviceVideoVirtualCamera.deviceHandle, g_UsbDeviceVideoVirtualCamera.speed);
            }

            if (USB_SPEED_HIGH == g_UsbDeviceVideoVirtualCamera.speed)
            {
                g_UsbDeviceVideoVirtualCamera.currentMaxPacketSize = HS_STREAM_IN_PACKET_SIZE;
            }
#endif
            error = kStatus_USB_Success;
        }
        break;
        case kUSB_DeviceEventSetConfiguration:
            if (0 == (*temp8))
            {
                g_UsbDeviceVideoVirtualCamera.attach               = 0U;
                g_UsbDeviceVideoVirtualCamera.currentConfiguration = 0U;
                error                                              = kStatus_USB_Success;
            }
            else if (USB_VIDEO_VIRTUAL_CAMERA_CONFIGURE_INDEX == (*temp8))
            {
                /* Set the configuration request */
                g_UsbDeviceVideoVirtualCamera.attach               = 1U;
                g_UsbDeviceVideoVirtualCamera.currentConfiguration = *temp8;
                error                                              = kStatus_USB_Success;
            }
            else
            {
                /* no action */
            }
            break;
        case kUSB_DeviceEventSetInterface:
            if ((g_UsbDeviceVideoVirtualCamera.attach) && param)
            {
                /* Set alternateSetting of the interface request */
                uint8_t interface        = (uint8_t)((*temp16 & 0xFF00U) >> 0x08U);
                uint8_t alternateSetting = (uint8_t)(*temp16 & 0x00FFU);

                if (USB_VIDEO_VIRTUAL_CAMERA_CONTROL_INTERFACE_INDEX == interface)
                {
                    if (alternateSetting < USB_VIDEO_VIRTUAL_CAMERA_CONTROL_INTERFACE_ALTERNATE_COUNT)
                    {
                        g_UsbDeviceVideoVirtualCamera.currentInterfaceAlternateSetting[interface] = alternateSetting;
                        error = kStatus_USB_Success;
                    }
                }
                else if (USB_VIDEO_VIRTUAL_CAMERA_STREAM_INTERFACE_INDEX == interface)
                {
                    if (alternateSetting < USB_VIDEO_VIRTUAL_CAMERA_STREAM_INTERFACE_ALTERNATE_COUNT)
                    {
                        if (USB_VIDEO_VIRTUAL_CAMERA_STREAM_INTERFACE_ALTERNATE_0 ==
                            g_UsbDeviceVideoVirtualCamera.currentInterfaceAlternateSetting[interface])
                        {
                            USB_DeviceVideoPrepareVideoData();
                            error = USB_DeviceSendRequest(g_UsbDeviceVideoVirtualCamera.deviceHandle,
                                                          USB_VIDEO_VIRTUAL_CAMERA_STREAM_ENDPOINT_IN,
                                                          g_UsbDeviceVideoVirtualCamera.imageBuffer,
                                                          g_UsbDeviceVideoVirtualCamera.imageBufferLength);
                        }
                        else
                        {
                            error = kStatus_USB_Success;
                        }
                        g_UsbDeviceVideoVirtualCamera.currentInterfaceAlternateSetting[interface] = alternateSetting;
                    }
                }
                else
                {
                    /* no action */
                }
            }
            break;
        case kUSB_DeviceEventGetConfiguration:
            if (param)
            {
                /* Get the current configuration request */
                *temp8 = g_UsbDeviceVideoVirtualCamera.currentConfiguration;
                error  = kStatus_USB_Success;
            }
            break;
        case kUSB_DeviceEventGetInterface:
            if (param)
            {
                /* Set the alternateSetting of the interface request */
                uint8_t interface = (uint8_t)((*temp16 & 0xFF00U) >> 0x08U);
                if (interface < USB_VIDEO_VIRTUAL_CAMERA_INTERFACE_COUNT)
                {
                    *temp16 =
                        (*temp16 & 0xFF00U) | g_UsbDeviceVideoVirtualCamera.currentInterfaceAlternateSetting[interface];
                    error = kStatus_USB_Success;
                }
            }
            break;
        case kUSB_DeviceEventGetDeviceDescriptor:
            if (param)
            {
                /* Get the device descriptor request */
                error = USB_DeviceGetDeviceDescriptor(handle, (usb_device_get_device_descriptor_struct_t *)param);
            }
            break;
        case kUSB_DeviceEventGetConfigurationDescriptor:
            if (param)
            {
                /* Get the configuration descriptor request */
                error = USB_DeviceGetConfigurationDescriptor(handle,
                                                             (usb_device_get_configuration_descriptor_struct_t *)param);
            }
            break;
        case kUSB_DeviceEventGetStringDescriptor:
            if (param)
            {
                /* Get the string descriptor request */
                error = USB_DeviceGetStringDescriptor(handle, (usb_device_get_string_descriptor_struct_t *)param);
            }
            break;
        default:
            /* no action */
            break;
    }

    return error;
}

void USB_VideoCamera_Init(void)
{
    /* USB_DeviceClockInit() already ran from main() - after CAMERA_CAPTURE_Deinit() cleanly
     * stopped SmartDMA and raised DCDC to Overdrive - see WORKLOG.md. */
#if (defined(FSL_FEATURE_SOC_SYSMPU_COUNT) && (FSL_FEATURE_SOC_SYSMPU_COUNT > 0U))
    SYSMPU_Enable(SYSMPU, 0);
#endif /* FSL_FEATURE_SOC_SYSMPU_COUNT */

    USB_DeviceVideoApplicationSetDefault();

    if (kStatus_USB_Success !=
        USB_DeviceClassInit(CONTROLLER_ID, &g_UsbDeviceVideoConfigList, &g_UsbDeviceVideoVirtualCamera.deviceHandle))
    {
        PRINTF("USB: video class init failed\r\n");
        return;
    }
    else
    {
        PRINTF("USB: video class (UVC) camera ready - 320x240 YUY2 @ 30fps over USB HS\r\n");
        g_UsbDeviceVideoVirtualCamera.videoHandle = g_UsbDeviceVideoConfigList.config->classHandle;
    }

    USB_DeviceIsrEnable();

    /* Add one delay here to make the DP pull down long enough to allow host to detect the previous disconnection. */
    SDK_DelayAtLeastUs(5000, SDK_DEVICE_MAXIMUM_CPU_CLOCK_FREQUENCY);
    USB_DeviceRun(g_UsbDeviceVideoVirtualCamera.deviceHandle);
}

void USB_VideoCamera_Task(void)
{
#if USB_DEVICE_CONFIG_USE_TASK
    USB_DeviceTaskFn(g_UsbDeviceVideoVirtualCamera.deviceHandle);
#endif
}
