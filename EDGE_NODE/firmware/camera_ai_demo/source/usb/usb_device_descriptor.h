/*
 * usb_device_descriptor.h - Camera_AI_Test1 USB Video Class (UVC)
 * descriptors. Abandoned path - see WORKLOG.md.
 *
 * Adapted from mcuxsdk's usb_device_video_virtual_camera example,
 * retargeted from a synthetic 176x144 MJPEG stream to a 320x240
 * *uncompressed* YUY2 stream from the real OV7670 camera - YUY2 because
 * MCXN947 has no hardware JPEG encoder and the camera path produces raw
 * RGB565. Identifiers still say "VIRTUAL_CAMERA" in places because that's
 * what usb_video_camera.c's class driver plumbing expects - cosmetic, not
 * functional.
 */

#ifndef __USB_DEVICE_DESCRIPTOR_H__
#define __USB_DEVICE_DESCRIPTOR_H__

/*******************************************************************************
 * Definitions
 ******************************************************************************/

#define USB_DEVICE_SPECIFIC_BCD_VERSION (0x0200U)
#define USB_DEVICE_DEMO_BCD_VERSION (0x0101U)

#define USB_DEVICE_VID (0x1FC9U)
#define USB_DEVICE_PID (0x009AU) /* one off from the stock example's 0x0099, so this doesn't collide on a host that has both plugged in */

#define USB_DEVICE_CLASS (0xEFU)
#define USB_DEVICE_SUBCLASS (0x02U)
#define USB_DEVICE_PROTOCOL (0x01U)

#define USB_DEVICE_MAX_POWER (0x32U)

/* No USB_DEVICE_VIDEO_CLASS_VERSION_1_1 / _1_5 defined -> UVC 1.0 (bcdUVC 0x0100), same as the stock example's default build. */
#define USB_DEVICE_VIDEO_SPECIFIC_BCD_VERSION (0x0100U)
#define USB_DEVICE_VIDEO_VIRTUAL_CAMERA_PROTOCOL USB_DEVICE_VIDEO_PC_PROTOCOL_UNDEFINED
#define USB_DESCRIPTOR_LENGTH_CONFIGURATION_ALL (sizeof(g_UsbDeviceConfigurationDescriptor))

#define USB_DESCRIPTOR_LENGTH_INTERFACE_ASSOCIATION (0x08U)
#define USB_DESCRIPTOR_LENGTH_STRING0 (sizeof(g_UsbDeviceString0))
#define USB_DESCRIPTOR_LENGTH_STRING1 (sizeof(g_UsbDeviceString1))
#define USB_DESCRIPTOR_LENGTH_STRING2 (sizeof(g_UsbDeviceString2))
#define USB_DESCRIPTOR_LENGTH_STRING3 (sizeof(g_UsbDeviceString3))

#define USB_DEVICE_CONFIGURATION_COUNT (1U)
#define USB_DEVICE_STRING_COUNT (4U)
#define USB_DEVICE_LANGUAGE_COUNT (1U)

#define USB_VIDEO_VIRTUAL_CAMERA_CONFIGURE_INDEX (1U)

#define USB_VIDEO_VIRTUAL_CAMERA_CONTROL_INTERFACE_COUNT (1U)
#define USB_VIDEO_VIRTUAL_CAMERA_CONTROL_INTERFACE_INDEX (0U)
#define USB_VIDEO_VIRTUAL_CAMERA_CONTROL_INTERFACE_ALTERNATE_COUNT (1U)
#define USB_VIDEO_VIRTUAL_CAMERA_CONTROL_INTERFACE_ALTERNATE_0 (0U)
#define USB_VIDEO_VIRTUAL_CAMERA_CONTROL_ENDPOINT_COUNT (1U)
#define USB_VIDEO_VIRTUAL_CAMERA_CONTROL_ENDPOINT (1U)

#define HS_INTERRUPT_IN_PACKET_SIZE (8U)
#define FS_INTERRUPT_IN_PACKET_SIZE (8U)
#define HS_INTERRUPT_IN_INTERVAL (0x07U) /* 2^(7-1) = 8ms */
#define FS_INTERRUPT_IN_INTERVAL (0x08U)

#define USB_VIDEO_VIRTUAL_CAMERA_VC_INTERFACE_HEADER_LENGTH (0x0DU)
#define USB_VIDEO_VIRTUAL_CAMERA_VC_OUTPUT_TERMINAL_LENGTH (0x09U)

#define USB_VIDEO_VIRTUAL_CAMERA_VC_PROCESSING_UNIT_LENGTH (0x0BU)
#define USB_VIDEO_VIRTUAL_CAMERA_VC_INPUT_TERMINAL_LENGTH (0x12U)

#define USB_VIDEO_VIRTUAL_CAMERA_VC_INTERFACE_TOTAL_LENGTH                                                     \
    (USB_VIDEO_VIRTUAL_CAMERA_VC_INTERFACE_HEADER_LENGTH + USB_VIDEO_VIRTUAL_CAMERA_VC_INPUT_TERMINAL_LENGTH + \
     USB_VIDEO_VIRTUAL_CAMERA_VC_OUTPUT_TERMINAL_LENGTH + USB_VIDEO_VIRTUAL_CAMERA_VC_PROCESSING_UNIT_LENGTH)

#define USB_VIDEO_VIRTUAL_CAMERA_CLOCK_FREQUENCY (6000000U) /* 6MHz */

#define USB_VIDEO_VIRTUAL_CAMERA_STREAM_INTERFACE_COUNT (1U)
#define USB_VIDEO_VIRTUAL_CAMERA_STREAM_INTERFACE_INDEX (1U)
#define USB_VIDEO_VIRTUAL_CAMERA_STREAM_INTERFACE_ALTERNATE_COUNT (2U)
#define USB_VIDEO_VIRTUAL_CAMERA_STREAM_INTERFACE_ALTERNATE_0 (0U)
#define USB_VIDEO_VIRTUAL_CAMERA_STREAM_INTERFACE_ALTERNATE_1 (1U)
#define USB_VIDEO_VIRTUAL_CAMERA_STREAM_ENDPOINT_COUNT (1U)
#define USB_VIDEO_VIRTUAL_CAMERA_STREAM_ENDPOINT_IN (2U)

/* 1024B/microframe is the largest packet a HS isochronous endpoint can use without the
 * "extra transactions per microframe" high-bandwidth bits - ~65.5 Mbit/s, comfortably above
 * the ~36.9 Mbit/s a 320x240 YUY2 stream needs at 30fps, so one transaction is enough. */
#define HS_STREAM_IN_PACKET_SIZE (1024U)
#define FS_STREAM_IN_PACKET_SIZE (512U)
#define HS_STREAM_IN_INTERVAL (0x04U) /* 2^(4-1) = 1ms */
#define FS_STREAM_IN_INTERVAL (0x01U)

#define USB_VIDEO_VIRTUAL_CAMERA_VS_INTERFACE_HEADER_LENGTH (0x0EU)
#define USB_VIDEO_UNCOMPRESSED_FORMAT_DESCRIPTOR_LENGTH (0x1BU) /* 27: fixed fields + 16-byte GUID */
#define USB_VIDEO_UNCOMPRESSED_FRAME_DESCRIPTOR_LENGTH (0x1EU)  /* 30: 26 fixed fields + 1 discrete dwFrameInterval */
#define USB_VIDEO_VIRTUAL_CAMERA_VS_INTERFACE_TOTAL_LENGTH                                                       \
    (USB_VIDEO_VIRTUAL_CAMERA_VS_INTERFACE_HEADER_LENGTH + USB_VIDEO_UNCOMPRESSED_FORMAT_DESCRIPTOR_LENGTH + \
     USB_VIDEO_UNCOMPRESSED_FRAME_DESCRIPTOR_LENGTH)

#define USB_VIDEO_VIRTUAL_CAMERA_INTERFACE_COUNT \
    (USB_VIDEO_VIRTUAL_CAMERA_CONTROL_INTERFACE_COUNT + USB_VIDEO_VIRTUAL_CAMERA_STREAM_INTERFACE_COUNT)

/* Stream format */
#define USB_VIDEO_VIRTUAL_CAMERA_FORMAT_COUNT (1U)
#define USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FORMAT_INDEX (1U) /* name kept for the class driver's probe/commit plumbing; format itself is uncompressed YUY2, not MJPEG */

#define USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_COUNT (1U)
#define USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_INDEX (1U)

/* Real camera resolution (see DEMO_BUFFER_WIDTH/HEIGHT in app.h) - the OV7670/SmartDMA capture is fixed at this size. */
#define USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_WIDTH (320U)
#define USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_HEIGHT (240U)

/* Only one frame interval offered, matching the camera's fixed 30fps capture rate (see camera_capture.c) - no point
 * advertising rates the source can't actually produce. */
#define USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_INTERVAL_TYPE (1U)
#define USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_INTERVAL_30FPS (10000000U / 30U)

#define USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_DEFAULT_INTERVAL USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_INTERVAL_30FPS
#define USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_MIN_INTERVAL USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_INTERVAL_30FPS
#define USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_MAX_INTERVAL USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_INTERVAL_30FPS

/* Uncompressed video has a fixed (not variable, like JPEG) bit rate: frame size in bits * fps. Min == max. */
#define USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_MIN_BIT_RATE                                             \
    (16U * USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_WIDTH * USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_HEIGHT * \
     (10000000U / USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_MAX_INTERVAL))
#define USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_MAX_BIT_RATE USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_MIN_BIT_RATE

/* YUY2 is 16 bits/pixel, same as this formula already assumed. */
#define USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_MAX_FRAME_SIZE \
    (2U * USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_WIDTH * USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_HEIGHT)

/* terminal unit ID */
#define USB_VIDEO_VIRTUAL_CAMERA_VC_INPUT_TERMINAL_ID (1U)
#define USB_VIDEO_VIRTUAL_CAMERA_VC_INPUT_TERMINAL_TYPE (USB_DEVICE_VIDEO_ITT_CAMERA)
#define USB_VIDEO_VIRTUAL_CAMERA_VC_OUTPUT_TERMINAL_ID (2U)
#define USB_VIDEO_VIRTUAL_CAMERA_VC_PROCESSING_UNIT_ID (3U)

/*******************************************************************************
 * API
 ******************************************************************************/

/* Configure the device according to the USB speed. */
extern usb_status_t USB_DeviceSetSpeed(usb_device_handle handle, uint8_t speed);

/* Get device descriptor request */
usb_status_t USB_DeviceGetDeviceDescriptor(usb_device_handle handle,
                                           usb_device_get_device_descriptor_struct_t *deviceDescriptor);

/* Get device configuration descriptor request */
usb_status_t USB_DeviceGetConfigurationDescriptor(
    usb_device_handle handle, usb_device_get_configuration_descriptor_struct_t *configurationDescriptor);

/* Get device string descriptor request */
usb_status_t USB_DeviceGetStringDescriptor(usb_device_handle handle,
                                           usb_device_get_string_descriptor_struct_t *stringDescriptor);

#endif /* __USB_DEVICE_DESCRIPTOR_H__ */
