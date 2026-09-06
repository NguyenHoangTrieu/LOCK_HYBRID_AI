/*
 * usb_device_descriptor.c - see usb_device_descriptor.h
 *
 * VC (video control) side is unchanged UVC boilerplate from the SDK
 * example. VS (video streaming) format/frame descriptors are rewritten
 * for uncompressed YUY2 320x240 instead of MJPEG 176x144, and the
 * still-image-capture descriptor is dropped (unsupported by this device).
 */

#include "usb_device_config.h"
#include "usb.h"
#include "usb_device.h"

#include "usb_device_class.h"
#include "usb_device_video.h"

#include "usb_device_descriptor.h"
#include "usb_video_camera.h"

/*******************************************************************************
 * Variables
 ******************************************************************************/

/* Video control endpoint information */
usb_device_endpoint_struct_t g_UsbDeviceVideoControlEndpoints[USB_VIDEO_VIRTUAL_CAMERA_CONTROL_ENDPOINT_COUNT] = {
    {
        USB_VIDEO_VIRTUAL_CAMERA_CONTROL_ENDPOINT | (USB_IN << USB_DESCRIPTOR_ENDPOINT_ADDRESS_DIRECTION_SHIFT),
        USB_ENDPOINT_INTERRUPT,
        FS_INTERRUPT_IN_PACKET_SIZE,
        FS_INTERRUPT_IN_INTERVAL,
    },
};

/* Video device entities */
usb_device_video_entity_struct_t g_UsbDeviceVideoEntity[] = {
    {
        USB_VIDEO_VIRTUAL_CAMERA_VC_INPUT_TERMINAL_ID,   /* The ID of input terminal */
        USB_DESCRIPTOR_SUBTYPE_VIDEO_VC_INPUT_TERMINAL,  /* Entity sub type type is VC input terminal */
        USB_VIDEO_VIRTUAL_CAMERA_VC_INPUT_TERMINAL_TYPE, /* VC intput terminal type */
    },
    {
        USB_VIDEO_VIRTUAL_CAMERA_VC_OUTPUT_TERMINAL_ID,  /* The ID of output terminal */
        USB_DESCRIPTOR_SUBTYPE_VIDEO_VC_OUTPUT_TERMINAL, /* Entity sub type type is VC output terminal */
        0U,
    },
    {
        USB_VIDEO_VIRTUAL_CAMERA_VC_PROCESSING_UNIT_ID,  /* The ID of processing terminal */
        USB_DESCRIPTOR_SUBTYPE_VIDEO_VC_PROCESSING_UNIT, /* Entity sub type type is VC processing terminal */
        0U,
    },
};

/* Video device enetitis list */
usb_device_video_entities_struct_t g_UsbDeviceVideoEntities = {
    g_UsbDeviceVideoEntity,
    sizeof(g_UsbDeviceVideoEntity) / sizeof(usb_device_video_entity_struct_t),
};

/* Video device control interface */
usb_device_interface_struct_t g_UsbDeviceVideoControlInterface[] = {{
    USB_VIDEO_VIRTUAL_CAMERA_CONTROL_INTERFACE_ALTERNATE_0, /* Alternate setting value */
    {
        USB_VIDEO_VIRTUAL_CAMERA_CONTROL_ENDPOINT_COUNT, /* control endpoint count */
        g_UsbDeviceVideoControlEndpoints,                /* control endpoint list */
    },
    &g_UsbDeviceVideoEntities, /* video device entitis list pointer */
}};

/* Video stream endpoint information */
usb_device_endpoint_struct_t g_UsbDeviceVideoStreamEndpoints[USB_VIDEO_VIRTUAL_CAMERA_STREAM_ENDPOINT_COUNT] = {
    {
        USB_VIDEO_VIRTUAL_CAMERA_STREAM_ENDPOINT_IN | (USB_IN << USB_DESCRIPTOR_ENDPOINT_ADDRESS_DIRECTION_SHIFT),
        USB_ENDPOINT_ISOCHRONOUS,
        FS_STREAM_IN_PACKET_SIZE,
        FS_STREAM_IN_INTERVAL,
    },
};

/* Video device stream interface */
usb_device_interface_struct_t g_UsbDeviceVideoStreamInterface[] = {
    {
        USB_VIDEO_VIRTUAL_CAMERA_STREAM_INTERFACE_ALTERNATE_0, /* Alternate setting value is zero*/
        {
            0U, /* endpoint count is zero for this alternate setting */
            NULL,
        },
        NULL,
    },
    {
        USB_VIDEO_VIRTUAL_CAMERA_STREAM_INTERFACE_ALTERNATE_1, /* Alternate setting value is one */
        {
            USB_VIDEO_VIRTUAL_CAMERA_STREAM_ENDPOINT_COUNT, /* endpoint count for this alternate setting */
            g_UsbDeviceVideoStreamEndpoints,                /* stream endpoint list */
        },
        NULL,
    },
};

/* The video device interfaces */
usb_device_interfaces_struct_t g_UsbDeviceVideoInterfaces[USB_VIDEO_VIRTUAL_CAMERA_INTERFACE_COUNT] = {
    {
        USB_DEVICE_VIDEO_CC_VIDEO,                        /* Class code */
        USB_DEVICE_VIDEO_SC_VIDEOCONTROL,                 /* Subclass code */
        USB_DEVICE_VIDEO_PC_PROTOCOL_UNDEFINED,           /* Protocol code */
        USB_VIDEO_VIRTUAL_CAMERA_CONTROL_INTERFACE_INDEX, /* The control interface index */
        g_UsbDeviceVideoControlInterface,                 /* control interface list */
        sizeof(g_UsbDeviceVideoControlInterface) / sizeof(usb_device_interface_struct_t),
    },
    {
        USB_DEVICE_VIDEO_CC_VIDEO,                       /* Class code */
        USB_DEVICE_VIDEO_SC_VIDEOSTREAMING,              /* Subclass code */
        USB_DEVICE_VIDEO_PC_PROTOCOL_UNDEFINED,          /* Protocol code */
        USB_VIDEO_VIRTUAL_CAMERA_STREAM_INTERFACE_INDEX, /* The stream interface index */
        g_UsbDeviceVideoStreamInterface,                 /* stream interface list */
        sizeof(g_UsbDeviceVideoStreamInterface) / sizeof(usb_device_interface_struct_t),
    },
};

/* The video device interface list */
usb_device_interface_list_t g_UsbDeviceVideoInterfaceList[USB_DEVICE_CONFIGURATION_COUNT] = {
    {
        USB_VIDEO_VIRTUAL_CAMERA_INTERFACE_COUNT, /* the interface count */
        g_UsbDeviceVideoInterfaces,               /* the control and stream interfaces pointer */
    },
};

/* The video device all interface list */
usb_device_class_struct_t g_UsbDeviceVideoVirtualCameraConfig = {
    g_UsbDeviceVideoInterfaceList,
    kUSB_DeviceClassTypeVideo,
    USB_DEVICE_CONFIGURATION_COUNT,
};

USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
uint8_t g_UsbDeviceDescriptor[] = {
    USB_DESCRIPTOR_LENGTH_DEVICE, /* Size of this descriptor in bytes */
    USB_DESCRIPTOR_TYPE_DEVICE,   /* DEVICE Descriptor Type */
    USB_SHORT_GET_LOW(USB_DEVICE_SPECIFIC_BCD_VERSION),
    USB_SHORT_GET_HIGH(USB_DEVICE_SPECIFIC_BCD_VERSION), /* USB Specification Release Number in
                                                            Binary-Coded Decimal (i.e., 2.10 is 210H). */
    USB_DEVICE_CLASS,                                    /* Class code (assigned by the USB-IF). */
    USB_DEVICE_SUBCLASS,                                 /* Subclass code (assigned by the USB-IF). */
    USB_DEVICE_PROTOCOL,                                 /* Protocol code (assigned by the USB-IF). */
    USB_CONTROL_MAX_PACKET_SIZE,                         /* Maximum packet size for endpoint zero
                                                            (only 8, 16, 32, or 64 are valid) */
    USB_SHORT_GET_LOW(USB_DEVICE_VID),
    USB_SHORT_GET_HIGH(USB_DEVICE_VID), /* Vendor ID (assigned by the USB-IF) */
    USB_SHORT_GET_LOW(USB_DEVICE_PID),
    USB_SHORT_GET_HIGH(USB_DEVICE_PID), /* Product ID (assigned by the manufacturer) */
    USB_SHORT_GET_LOW(USB_DEVICE_DEMO_BCD_VERSION),
    USB_SHORT_GET_HIGH(USB_DEVICE_DEMO_BCD_VERSION), /* Device release number in binary-coded decimal */
    0x01U,                                           /* Index of string descriptor describing manufacturer */
    0x02U,                                           /* Index of string descriptor describing product */
    0x00U,                                           /* Index of string descriptor describing the
                                                        device's serial number */
    USB_DEVICE_CONFIGURATION_COUNT,                  /* Number of possible configurations */
};

USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
uint8_t g_UsbDeviceConfigurationDescriptor[] = {
    USB_DESCRIPTOR_LENGTH_CONFIGURE, /* Size of this descriptor in bytes */
    USB_DESCRIPTOR_TYPE_CONFIGURE,   /* CONFIGURATION Descriptor Type */
    USB_SHORT_GET_LOW(USB_DESCRIPTOR_LENGTH_CONFIGURE + USB_DESCRIPTOR_LENGTH_INTERFACE_ASSOCIATION +
                      USB_DESCRIPTOR_LENGTH_INTERFACE + USB_VIDEO_VIRTUAL_CAMERA_VC_INTERFACE_HEADER_LENGTH +
                      USB_VIDEO_VIRTUAL_CAMERA_VC_INPUT_TERMINAL_LENGTH +
                      USB_VIDEO_VIRTUAL_CAMERA_VC_OUTPUT_TERMINAL_LENGTH +
                      USB_VIDEO_VIRTUAL_CAMERA_VC_PROCESSING_UNIT_LENGTH + USB_DESCRIPTOR_LENGTH_ENDPOINT +
                      USB_DESCRIPTOR_LENGTH_INTERFACE + USB_VIDEO_VIRTUAL_CAMERA_VS_INTERFACE_HEADER_LENGTH +
                      USB_VIDEO_UNCOMPRESSED_FORMAT_DESCRIPTOR_LENGTH + USB_VIDEO_UNCOMPRESSED_FRAME_DESCRIPTOR_LENGTH +
                      USB_DESCRIPTOR_LENGTH_INTERFACE + USB_DESCRIPTOR_LENGTH_ENDPOINT),
    USB_SHORT_GET_HIGH(USB_DESCRIPTOR_LENGTH_CONFIGURE + USB_DESCRIPTOR_LENGTH_INTERFACE_ASSOCIATION +
                       USB_DESCRIPTOR_LENGTH_INTERFACE + USB_VIDEO_VIRTUAL_CAMERA_VC_INTERFACE_HEADER_LENGTH +
                       USB_VIDEO_VIRTUAL_CAMERA_VC_INPUT_TERMINAL_LENGTH +
                       USB_VIDEO_VIRTUAL_CAMERA_VC_OUTPUT_TERMINAL_LENGTH +
                       USB_VIDEO_VIRTUAL_CAMERA_VC_PROCESSING_UNIT_LENGTH + USB_DESCRIPTOR_LENGTH_ENDPOINT +
                       USB_DESCRIPTOR_LENGTH_INTERFACE + USB_VIDEO_VIRTUAL_CAMERA_VS_INTERFACE_HEADER_LENGTH +
                       USB_VIDEO_UNCOMPRESSED_FORMAT_DESCRIPTOR_LENGTH + USB_VIDEO_UNCOMPRESSED_FRAME_DESCRIPTOR_LENGTH +
                       USB_DESCRIPTOR_LENGTH_INTERFACE +
                       USB_DESCRIPTOR_LENGTH_ENDPOINT), /* Total length of data returned for this configuration. */
    USB_VIDEO_VIRTUAL_CAMERA_INTERFACE_COUNT,           /* Number of interfaces supported by this configuration */
    USB_VIDEO_VIRTUAL_CAMERA_CONFIGURE_INDEX,           /* Value to use as an argument to the
                                                 SetConfiguration() request to select this configuration */
    0x00U,                                              /* Index of string descriptor describing this configuration */
    (USB_DESCRIPTOR_CONFIGURE_ATTRIBUTE_D7_MASK) |
#if defined(USB_DEVICE_CONFIG_SELF_POWER) && (USB_DEVICE_CONFIG_SELF_POWER > 0U)
    (1U << USB_DESCRIPTOR_CONFIGURE_ATTRIBUTE_SELF_POWERED_SHIFT) |
#endif
#if defined(USB_DEVICE_CONFIG_REMOTE_WAKEUP) && (USB_DEVICE_CONFIG_REMOTE_WAKEUP > 0U)
    (1U << USB_DESCRIPTOR_CONFIGURE_ATTRIBUTE_REMOTE_WAKEUP_SHIFT) |
#endif
     0U,
    /* Configuration characteristics
        D7: Reserved (set to one)
        D6: Self-powered
        D5: Remote Wakeup
        D4...0: Reserved (reset to zero)
     */
    USB_DEVICE_MAX_POWER, /* Maximum power consumption of the USB
                           * device from the bus in this specific
                           * configuration when the device is fully
                           * operational. Expressed in 2 mA units
                           *  (i.e., 50 = 100 mA).
                           */
    /* Interface Association Descriptor */
    USB_DESCRIPTOR_LENGTH_INTERFACE_ASSOCIATION,      /* size of the IAD */
    USB_DESCRIPTOR_TYPE_INTERFACE_ASSOCIATION,        /* INTERFACE ASSOCIATION Descriptor */
    USB_VIDEO_VIRTUAL_CAMERA_CONTROL_INTERFACE_INDEX, /* Interface number of the first VideoControl
                                                         interface that is associated with this function. */
    USB_VIDEO_VIRTUAL_CAMERA_INTERFACE_COUNT,         /* Number of contiguous VideoStreaming interfaces that are
                                                         associated with this function.
                                                         The count includes the first VideoControl interface and all
                                                         its associated VideoStreaming interfaces.
                                                      */
    USB_DEVICE_VIDEO_CC_VIDEO,                        /* CC_VIDEO, Video Interface Class code */
    USB_DEVICE_VIDEO_SC_VIDEO_INTERFACE_COLLECTION,   /* SC_VIDEO_INTERFACE_COLLECTION. Video Interface
                                                         Subclass code */
    USB_DEVICE_VIDEO_PC_PROTOCOL_UNDEFINED,           /* Not used, Must be set to PC_PROTOCOL_UNDEFINED */
    0x03U,                                            /* Index of a string descriptor */

    /* Standard VC Interface Descriptor */
    USB_DESCRIPTOR_LENGTH_INTERFACE,                  /* Size of this descriptor */
    USB_DESCRIPTOR_TYPE_INTERFACE,                    /* INTERFACE Descriptor */
    USB_VIDEO_VIRTUAL_CAMERA_CONTROL_INTERFACE_INDEX, /* Index of control interface */
    USB_VIDEO_VIRTUAL_CAMERA_CONTROL_INTERFACE_ALTERNATE_0, /* Index of the interface setting */
    USB_VIDEO_VIRTUAL_CAMERA_CONTROL_ENDPOINT_COUNT,  /* One endpoint of control pipe */
    USB_DEVICE_VIDEO_CC_VIDEO,                        /* CC_VIDEO */
    USB_DEVICE_VIDEO_SC_VIDEOCONTROL,                 /* SC_VIDEOCONTROL */
    USB_DEVICE_VIDEO_VIRTUAL_CAMERA_PROTOCOL,         /* Protocol */
    0x00U,                                            /* Index of this string descriptor */

    /* Class-Specific VC Interface header Descriptor */
    USB_VIDEO_VIRTUAL_CAMERA_VC_INTERFACE_HEADER_LENGTH, /* Size of this descriptor, 12+n */
    USB_DESCRIPTOR_TYPE_VIDEO_CS_INTERFACE,              /* CS_INTERFACE, descriptor type */
    USB_DESCRIPTOR_SUBTYPE_VIDEO_VC_HEADER,              /* VC_HEADER, descriptor subtype */
    USB_SHORT_GET_LOW(USB_DEVICE_VIDEO_SPECIFIC_BCD_VERSION), USB_SHORT_GET_HIGH(USB_DEVICE_VIDEO_SPECIFIC_BCD_VERSION),
    /* bcdUVC */
    USB_SHORT_GET_LOW(USB_VIDEO_VIRTUAL_CAMERA_VC_INTERFACE_TOTAL_LENGTH),
    USB_SHORT_GET_HIGH(USB_VIDEO_VIRTUAL_CAMERA_VC_INTERFACE_TOTAL_LENGTH),
    /* Total number of bytes returned for the class-specific VideoControl interface descriptor.
       Includes the combined length of this descriptor header and all Unit and Terminal descriptors. */
    USB_LONG_GET_BYTE0(USB_VIDEO_VIRTUAL_CAMERA_CLOCK_FREQUENCY),
    USB_LONG_GET_BYTE1(USB_VIDEO_VIRTUAL_CAMERA_CLOCK_FREQUENCY),
    USB_LONG_GET_BYTE2(USB_VIDEO_VIRTUAL_CAMERA_CLOCK_FREQUENCY),
    USB_LONG_GET_BYTE3(USB_VIDEO_VIRTUAL_CAMERA_CLOCK_FREQUENCY),
    /* Use of this field has been deprecated. */
    USB_VIDEO_VIRTUAL_CAMERA_STREAM_INTERFACE_COUNT, /* The number of VideoStreaming interfaces in the Video
                                                        Interface Collection to which this VideoControl
                                                        interface belongs: n */
    USB_VIDEO_VIRTUAL_CAMERA_STREAM_INTERFACE_INDEX, /* Interface number of the first VideoStreaming
                                                        interface in the Collection */

    /* Input Terminal Descriptor (Camera) */
    USB_VIDEO_VIRTUAL_CAMERA_VC_INPUT_TERMINAL_LENGTH, /* Size of this descriptor, 15+n */
    USB_DESCRIPTOR_TYPE_VIDEO_CS_INTERFACE,            /* CS_INTERFACE, descriptor type */
    USB_DESCRIPTOR_SUBTYPE_VIDEO_VC_INPUT_TERMINAL,    /* VC_INPUT_TERMINAL, descriptor subtype */
    USB_VIDEO_VIRTUAL_CAMERA_VC_INPUT_TERMINAL_ID,     /* A non-zero constant that uniquely identifies the
                                                          Terminal within the video function. This value is used
                                                          in all requests to address this Terminal. */
    USB_SHORT_GET_LOW(USB_VIDEO_VIRTUAL_CAMERA_VC_INPUT_TERMINAL_TYPE),
    USB_SHORT_GET_HIGH(USB_VIDEO_VIRTUAL_CAMERA_VC_INPUT_TERMINAL_TYPE),
    /* ITT_CAMERA, Constant that characterizes the type of Terminal */
    0x00U,               /* ID of the Output Terminal to which this Input Terminal is associated */
    0x00U,               /* Index of a string descriptor */
    0x00U, 0x00U,        /* The value of Lmin, unsupported */
    0x00U, 0x00U,        /* The value of Lmax, unsupported */
    0x00U, 0x00U,        /* The value of Locular, unsupported */
    0x03U,               /* Size in bytes of the bmControls field: n */
    0x00U, 0x00U, 0x00U, /* Not control supported */

    /* Output Terminal Descriptor */
    USB_VIDEO_VIRTUAL_CAMERA_VC_OUTPUT_TERMINAL_LENGTH, /* Size of this descriptor, 9+n */
    USB_DESCRIPTOR_TYPE_VIDEO_CS_INTERFACE,             /* CS_INTERFACE, descriptor type */
    USB_DESCRIPTOR_SUBTYPE_VIDEO_VC_OUTPUT_TERMINAL,    /* VC_OUTPUT_TERMINAL, descriptor subtype */
    USB_VIDEO_VIRTUAL_CAMERA_VC_OUTPUT_TERMINAL_ID,     /* A non-zero constant that uniquely identifies the
                                                           Terminal within the video function. This value is
                                                           used in all requests to address this Terminal. */
    USB_SHORT_GET_LOW(USB_DEVICE_VIDEO_TT_STREAMING), USB_SHORT_GET_HIGH(USB_DEVICE_VIDEO_TT_STREAMING),
    /* TT_STREAMING, Constant that characterizes the type of Terminal */
    0x00U, /* ID of the Input Terminal to which this Output Terminal is associated */
    USB_VIDEO_VIRTUAL_CAMERA_VC_INPUT_TERMINAL_ID, /* ID of the Unit or Terminal to which this Terminal is
                                                      connected */
    0x00U,                                         /* Index of a string descriptor */

    /* Processing Unit Descriptor */
    USB_VIDEO_VIRTUAL_CAMERA_VC_PROCESSING_UNIT_LENGTH, /* Size of this descriptor, 10+n */
    USB_DESCRIPTOR_TYPE_VIDEO_CS_INTERFACE,             /* CS_INTERFACE, descriptor type */
    USB_DESCRIPTOR_SUBTYPE_VIDEO_VC_PROCESSING_UNIT,    /* VC_PROCESSING_UNIT, descriptor subtype */
    USB_VIDEO_VIRTUAL_CAMERA_VC_PROCESSING_UNIT_ID,     /* A non-zero constant that uniquely identifies the
                                                           Terminal within the video function. This value is
                                                           used in all requests to address this Terminal. */
    USB_VIDEO_VIRTUAL_CAMERA_VC_INPUT_TERMINAL_ID,      /* ID of the Unit or Terminal to which this Terminal is
                                                           connected */
    0x00U, 0x00U,  /* This field indicates the maximum digital magnification, multiplied by 100U */
    0x02U,         /* Size of the bmControls field, in bytes: n */
    0x00U, 0x00U,  /* Not control supported */
    0x00U,         /* Index of a string descriptor */
    /* No bmVideoStandards byte here: that field only exists in UVC 1.1/1.5 Processing Unit
     * descriptors, and USB_VIDEO_VIRTUAL_CAMERA_VC_PROCESSING_UNIT_LENGTH (0x0B) is the UVC 1.0
     * length that excludes it. Adding it unconditionally (as a stray copy-paste once did) desyncs
     * this descriptor from its own bLength and corrupts the rest of the byte stream - see
     * WORKLOG.md for the hardware symptom this caused. */

    /* Standard VC Interrupt Endpoint Descriptor */
    USB_DESCRIPTOR_LENGTH_ENDPOINT, /* Size of this descriptor, in bytes: 7U */
    USB_DESCRIPTOR_TYPE_ENDPOINT,   /* ENDPOINT descriptor type */
    USB_VIDEO_VIRTUAL_CAMERA_CONTROL_ENDPOINT |
        (USB_IN << USB_DESCRIPTOR_ENDPOINT_ADDRESS_DIRECTION_SHIFT), /* Endpoint address */
    USB_ENDPOINT_INTERRUPT,                                          /* Transfer type */
    USB_SHORT_GET_LOW(FS_INTERRUPT_IN_PACKET_SIZE), USB_SHORT_GET_HIGH(FS_INTERRUPT_IN_PACKET_SIZE),
    /* Max Packet Size */
    FS_INTERRUPT_IN_INTERVAL, /* Interval */

    /* Standard VS Interface Descriptor */
    USB_DESCRIPTOR_LENGTH_INTERFACE,                 /* Size of this descriptor */
    USB_DESCRIPTOR_TYPE_INTERFACE,                   /* INTERFACE Descriptor */
    USB_VIDEO_VIRTUAL_CAMERA_STREAM_INTERFACE_INDEX, /* Index of stream interface */
    USB_VIDEO_VIRTUAL_CAMERA_STREAM_INTERFACE_ALTERNATE_0, /* Index of the interface setting */
    0U,                                              /* No endpoint of stream pipe */
    USB_DEVICE_VIDEO_CC_VIDEO,                       /* CC_VIDEO */
    USB_DEVICE_VIDEO_SC_VIDEOSTREAMING,              /* SC_VIDEOSTREAMING */
    USB_DEVICE_VIDEO_VIRTUAL_CAMERA_PROTOCOL,        /* Protocol */
    0x00U,                                           /* Index of this string descriptor */

    /* VS Specific Input Header Descriptor */
    USB_VIDEO_VIRTUAL_CAMERA_VS_INTERFACE_HEADER_LENGTH, /* Size of this descriptor, in bytes: 13+(p*n) */
    USB_DESCRIPTOR_TYPE_VIDEO_CS_INTERFACE,              /* CS_INTERFACE descriptor type */
    USB_DESCRIPTOR_SUBTYPE_VIDEO_VS_INPUT_HEADER,        /* VS_INPUT_HEADER descriptor subtype */
    USB_VIDEO_VIRTUAL_CAMERA_FORMAT_COUNT,               /* One format (p = 1U)*/
    USB_SHORT_GET_LOW(USB_VIDEO_VIRTUAL_CAMERA_VS_INTERFACE_TOTAL_LENGTH),
    USB_SHORT_GET_HIGH(USB_VIDEO_VIRTUAL_CAMERA_VS_INTERFACE_TOTAL_LENGTH),
    /* Total number of bytes returned for the class-specific VideoStreaming interface descriptors including
       this header descriptor */
    USB_VIDEO_VIRTUAL_CAMERA_STREAM_ENDPOINT_IN | (USB_IN << 0x07U),
    /* The address of the isochronous or bulk endpoint used for video data */
    0x00U,                                           /* Dynamic Format Change unsupported */
    USB_VIDEO_VIRTUAL_CAMERA_VC_OUTPUT_TERMINAL_ID,  /* The terminal ID of the Output Terminal to which the
                                                        video endpoint of this interface is connected */
    0x00U,                                           /* bStillCaptureMethod: 0 = no still capture support */
    0x00U,                                           /* bTriggerSupport: no hardware trigger */
    0x00U, /* Specifies how the host software shall respond to a hardware trigger interrupt event from this
             interface */
    0x01U, /* Size of each bmaControl(x) field */
    0x00U, /* Not used */

    /* Uncompressed Video Format Descriptor */
    USB_VIDEO_UNCOMPRESSED_FORMAT_DESCRIPTOR_LENGTH,     /* Size of this Descriptor, in bytes: 27U */
    USB_DESCRIPTOR_TYPE_VIDEO_CS_INTERFACE,              /* CS_INTERFACE descriptor type */
    USB_DESCRIPTOR_SUBTYPE_VIDEO_VS_FORMAT_UNCOMPRESSED, /* VS_FORMAT_UNCOMPRESSED descriptor subtype */
    USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FORMAT_INDEX,         /* Index of this Format Descriptor */
    USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_COUNT,          /* Number of Frame Descriptors following that correspond to
                                                            this format */
    /* guidFormat: YUY2 (MEDIASUBTYPE_YUY2), the FourCC "YUY2" followed by the fixed
     * 0000-0010-8000-00AA00389B71 suffix all uncompressed UVC format GUIDs share. */
    'Y', 'U', 'Y', '2', 0x00U, 0x00U, 0x10U, 0x00U, 0x80U, 0x00U, 0x00U, 0xAAU, 0x00U, 0x38U, 0x9BU, 0x71U,
    16U,   /* bBitsPerPixel: YUY2 is 16 bits/pixel */
    0x01U, /* bDefaultFrameIndex */
    0x00U, /* bAspectRatioX */
    0x00U, /* bAspectRatioY */
    0x00U, /* bmInterlaceFlags */
    0x00U, /* bCopyProtect */

    /* Uncompressed Video Frame Descriptor */
    USB_VIDEO_UNCOMPRESSED_FRAME_DESCRIPTOR_LENGTH,     /* Size of this Descriptor */
    USB_DESCRIPTOR_TYPE_VIDEO_CS_INTERFACE,             /* CS_INTERFACE descriptor type */
    USB_DESCRIPTOR_SUBTYPE_VIDEO_VS_FRAME_UNCOMPRESSED, /* VS_FRAME_UNCOMPRESSED descriptor subtype */
    USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_INDEX,         /* First (only) frame */
    0x00U,                                              /* bmCapabilities */
    USB_SHORT_GET_LOW(USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_WIDTH),
    USB_SHORT_GET_HIGH(USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_WIDTH),
    /* Width */
    USB_SHORT_GET_LOW(USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_HEIGHT),
    USB_SHORT_GET_HIGH(USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_HEIGHT),
    /* Height */
    USB_LONG_GET_BYTE0(USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_MIN_BIT_RATE),
    USB_LONG_GET_BYTE1(USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_MIN_BIT_RATE),
    USB_LONG_GET_BYTE2(USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_MIN_BIT_RATE),
    USB_LONG_GET_BYTE3(USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_MIN_BIT_RATE),
    /* Min bit Rate */
    USB_LONG_GET_BYTE0(USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_MAX_BIT_RATE),
    USB_LONG_GET_BYTE1(USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_MAX_BIT_RATE),
    USB_LONG_GET_BYTE2(USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_MAX_BIT_RATE),
    USB_LONG_GET_BYTE3(USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_MAX_BIT_RATE),
    /* Max bit Rate */
    USB_LONG_GET_BYTE0(USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_MAX_FRAME_SIZE),
    USB_LONG_GET_BYTE1(USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_MAX_FRAME_SIZE),
    USB_LONG_GET_BYTE2(USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_MAX_FRAME_SIZE),
    USB_LONG_GET_BYTE3(USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_MAX_FRAME_SIZE),
    /* Max Frame buffer size */
    USB_LONG_GET_BYTE0(USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_DEFAULT_INTERVAL),
    USB_LONG_GET_BYTE1(USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_DEFAULT_INTERVAL),
    USB_LONG_GET_BYTE2(USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_DEFAULT_INTERVAL),
    USB_LONG_GET_BYTE3(USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_DEFAULT_INTERVAL),
    /* Default Frame interval */
    USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_INTERVAL_TYPE, /* frame interval type: 1 discrete interval */
    USB_LONG_GET_BYTE0(USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_INTERVAL_30FPS),
    USB_LONG_GET_BYTE1(USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_INTERVAL_30FPS),
    USB_LONG_GET_BYTE2(USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_INTERVAL_30FPS),
    USB_LONG_GET_BYTE3(USB_VIDEO_VIRTUAL_CAMERA_MJPEG_FRAME_INTERVAL_30FPS),
    /* 30fps - the only rate offered, since that's the camera's fixed capture rate */

    /* Standard VS Interface Descriptor */
    USB_DESCRIPTOR_LENGTH_INTERFACE,                 /* Size of this descriptor */
    USB_DESCRIPTOR_TYPE_INTERFACE,                   /* INTERFACE Descriptor */
    USB_VIDEO_VIRTUAL_CAMERA_STREAM_INTERFACE_INDEX, /* Index of stream interface */
    USB_VIDEO_VIRTUAL_CAMERA_STREAM_INTERFACE_ALTERNATE_1, /* Index of the interface setting */
    USB_VIDEO_VIRTUAL_CAMERA_STREAM_ENDPOINT_COUNT,  /* One endpoint of stream pipe */
    USB_DEVICE_VIDEO_CC_VIDEO,                       /* CC_VIDEO */
    USB_DEVICE_VIDEO_SC_VIDEOSTREAMING,              /* SC_VIDEOSTREAMING */
    USB_DEVICE_VIDEO_VIRTUAL_CAMERA_PROTOCOL,        /* Protocol */
    0x00U,                                           /* Index of this string descriptor */

    /*Endpoint descriptor */
    USB_DESCRIPTOR_LENGTH_ENDPOINT, /* Size of this descriptor */
    USB_DESCRIPTOR_TYPE_ENDPOINT,   /* ENDPOINT Descriptor */
    USB_VIDEO_VIRTUAL_CAMERA_STREAM_ENDPOINT_IN | (USB_IN << USB_DESCRIPTOR_ENDPOINT_ADDRESS_DIRECTION_SHIFT),
    USB_ENDPOINT_ISOCHRONOUS | USB_DESCRIPTOR_ENDPOINT_ATTRIBUTE_SYNC_TYPE_ASYNC,
    USB_SHORT_GET_LOW(FS_STREAM_IN_PACKET_SIZE), USB_SHORT_GET_HIGH(FS_STREAM_IN_PACKET_SIZE),
    /* Max Packet Size */
    FS_STREAM_IN_INTERVAL, /* Interval */
};

USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
uint8_t g_UsbDeviceString0[] = {
    2U + 2U,
    USB_DESCRIPTOR_TYPE_STRING,
    0x09U,
    0x04U,
};

USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
uint8_t g_UsbDeviceString1[] = {
    2U + 2U * 18U, USB_DESCRIPTOR_TYPE_STRING,
    'N',           0x00U,
    'X',           0x00U,
    'P',           0x00U,
    ' ',           0x00U,
    'S',           0x00U,
    'E',           0x00U,
    'M',           0x00U,
    'I',           0x00U,
    'C',           0x00U,
    'O',           0x00U,
    'N',           0x00U,
    'D',           0x00U,
    'U',           0x00U,
    'C',           0x00U,
    'T',           0x00U,
    'O',           0x00U,
    'R',           0x00U,
    'S',           0x00U,
};

USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
uint8_t g_UsbDeviceString2[] = {
    2U + 2U * 15U, USB_DESCRIPTOR_TYPE_STRING,
    'C',           0x00U,
    'a',           0x00U,
    'm',           0x00U,
    'e',           0x00U,
    'r',           0x00U,
    'a',           0x00U,
    '_',           0x00U,
    'A',           0x00U,
    'I',           0x00U,
    '_',           0x00U,
    'T',           0x00U,
    'e',           0x00U,
    's',           0x00U,
    't',           0x00U,
    '1',           0x00U,
};

USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
uint8_t g_UsbDeviceString3[] = {
    2U + 2U * 12U, USB_DESCRIPTOR_TYPE_STRING,
    'O',           0x00U,
    'V',           0x00U,
    '7',           0x00U,
    '6',           0x00U,
    '7',           0x00U,
    '0',           0x00U,
    ' ',           0x00U,
    'o',           0x00U,
    'n',           0x00U,
    ' ',           0x00U,
    'J',           0x00U,
    '9',           0x00U,
};

uint32_t g_UsbDeviceStringDescriptorLength[USB_DEVICE_STRING_COUNT] = {
    sizeof(g_UsbDeviceString0),
    sizeof(g_UsbDeviceString1),
    sizeof(g_UsbDeviceString2),
    sizeof(g_UsbDeviceString3),
};

uint8_t *g_UsbDeviceStringDescriptorArray[USB_DEVICE_STRING_COUNT] = {
    g_UsbDeviceString0,
    g_UsbDeviceString1,
    g_UsbDeviceString2,
    g_UsbDeviceString3,
};

usb_language_t g_UsbDeviceLanguage[USB_DEVICE_LANGUAGE_COUNT] = {{
    g_UsbDeviceStringDescriptorArray,
    g_UsbDeviceStringDescriptorLength,
    (uint16_t)0x0409U,
}};

usb_language_list_t g_UsbDeviceLanguageList = {
    g_UsbDeviceString0,
    sizeof(g_UsbDeviceString0),
    g_UsbDeviceLanguage,
    USB_DEVICE_LANGUAGE_COUNT,
};

/*******************************************************************************
 * Code
 ******************************************************************************/

/* Get device descriptor request */
usb_status_t USB_DeviceGetDeviceDescriptor(usb_device_handle handle,
                                           usb_device_get_device_descriptor_struct_t *deviceDescriptor)
{
    deviceDescriptor->buffer = g_UsbDeviceDescriptor;
    deviceDescriptor->length = USB_DESCRIPTOR_LENGTH_DEVICE;
    return kStatus_USB_Success;
}

/* Get device configuration descriptor request */
usb_status_t USB_DeviceGetConfigurationDescriptor(
    usb_device_handle handle, usb_device_get_configuration_descriptor_struct_t *configurationDescriptor)
{
    if (USB_VIDEO_VIRTUAL_CAMERA_CONFIGURE_INDEX > configurationDescriptor->configuration)
    {
        configurationDescriptor->buffer = g_UsbDeviceConfigurationDescriptor;
        configurationDescriptor->length = USB_DESCRIPTOR_LENGTH_CONFIGURATION_ALL;
        return kStatus_USB_Success;
    }
    return kStatus_USB_InvalidRequest;
}

/* Get device string descriptor request */
usb_status_t USB_DeviceGetStringDescriptor(usb_device_handle handle,
                                           usb_device_get_string_descriptor_struct_t *stringDescriptor)
{
    if (stringDescriptor->stringIndex == 0U)
    {
        stringDescriptor->buffer = (uint8_t *)g_UsbDeviceLanguageList.languageString;
        stringDescriptor->length = g_UsbDeviceLanguageList.stringLength;
    }
    else
    {
        uint8_t languageId    = 0U;
        uint8_t languageIndex = USB_DEVICE_STRING_COUNT;

        for (; languageId < USB_DEVICE_LANGUAGE_COUNT; languageId++)
        {
            if (stringDescriptor->languageId == g_UsbDeviceLanguageList.languageList[languageId].languageId)
            {
                if (stringDescriptor->stringIndex < USB_DEVICE_STRING_COUNT)
                {
                    languageIndex = stringDescriptor->stringIndex;
                }
                break;
            }
        }

        if (USB_DEVICE_STRING_COUNT == languageIndex)
        {
            return kStatus_USB_InvalidRequest;
        }
        stringDescriptor->buffer = (uint8_t *)g_UsbDeviceLanguageList.languageList[languageId].string[languageIndex];
        stringDescriptor->length = g_UsbDeviceLanguageList.languageList[languageId].length[languageIndex];
    }
    return kStatus_USB_Success;
}

/* HS and FS descriptors differ, so device descriptors/configurations need to be updated to match
 * the current speed once negotiated (default is FS parameters). Updates endpoint max packet size,
 * interval, etc. */
usb_status_t USB_DeviceSetSpeed(usb_device_handle handle, uint8_t speed)
{
    usb_descriptor_union_t *descriptorHead;
    usb_descriptor_union_t *descriptorTail;

    descriptorHead = (usb_descriptor_union_t *)&g_UsbDeviceConfigurationDescriptor[0];
    descriptorTail =
        (usb_descriptor_union_t *)(&g_UsbDeviceConfigurationDescriptor[USB_DESCRIPTOR_LENGTH_CONFIGURATION_ALL - 1U]);

    while (descriptorHead < descriptorTail)
    {
        if (descriptorHead->common.bDescriptorType == USB_DESCRIPTOR_TYPE_ENDPOINT)
        {
            if (USB_SPEED_HIGH == speed)
            {
                if ((USB_VIDEO_VIRTUAL_CAMERA_STREAM_ENDPOINT_IN ==
                     (descriptorHead->endpoint.bEndpointAddress & USB_ENDPOINT_NUMBER_MASK)) &&
                    ((descriptorHead->endpoint.bEndpointAddress & USB_DESCRIPTOR_ENDPOINT_ADDRESS_DIRECTION_MASK) ==
                     USB_DESCRIPTOR_ENDPOINT_ADDRESS_DIRECTION_IN))
                {
                    descriptorHead->endpoint.bInterval = HS_STREAM_IN_INTERVAL;
                    USB_SHORT_TO_LITTLE_ENDIAN_ADDRESS(HS_STREAM_IN_PACKET_SIZE,
                                                       descriptorHead->endpoint.wMaxPacketSize);
                }
                else
                {
                    descriptorHead->endpoint.bInterval = HS_INTERRUPT_IN_INTERVAL;
                    USB_SHORT_TO_LITTLE_ENDIAN_ADDRESS(HS_INTERRUPT_IN_PACKET_SIZE,
                                                       descriptorHead->endpoint.wMaxPacketSize);
                }
            }
            else
            {
                if ((USB_VIDEO_VIRTUAL_CAMERA_STREAM_ENDPOINT_IN ==
                     (descriptorHead->endpoint.bEndpointAddress & USB_ENDPOINT_NUMBER_MASK)) &&
                    ((descriptorHead->endpoint.bEndpointAddress & USB_DESCRIPTOR_ENDPOINT_ADDRESS_DIRECTION_MASK) ==
                     USB_DESCRIPTOR_ENDPOINT_ADDRESS_DIRECTION_IN))
                {
                    descriptorHead->endpoint.bInterval = FS_STREAM_IN_INTERVAL;
                    USB_SHORT_TO_LITTLE_ENDIAN_ADDRESS(FS_STREAM_IN_PACKET_SIZE,
                                                       descriptorHead->endpoint.wMaxPacketSize);
                }
                else
                {
                    descriptorHead->endpoint.bInterval = FS_INTERRUPT_IN_INTERVAL;
                    USB_SHORT_TO_LITTLE_ENDIAN_ADDRESS(FS_INTERRUPT_IN_PACKET_SIZE,
                                                       descriptorHead->endpoint.wMaxPacketSize);
                }
            }
        }
        descriptorHead = (usb_descriptor_union_t *)((uint8_t *)descriptorHead + descriptorHead->common.bLength);
    }

    if (USB_SPEED_HIGH == speed)
    {
        g_UsbDeviceVideoControlEndpoints[0].maxPacketSize = HS_INTERRUPT_IN_PACKET_SIZE;
        g_UsbDeviceVideoStreamEndpoints[0].maxPacketSize  = HS_STREAM_IN_PACKET_SIZE;

        g_UsbDeviceVideoControlEndpoints[0].interval = HS_INTERRUPT_IN_INTERVAL;
        g_UsbDeviceVideoStreamEndpoints[0].interval  = HS_STREAM_IN_INTERVAL;
    }
    else
    {
        g_UsbDeviceVideoControlEndpoints[0].maxPacketSize = FS_INTERRUPT_IN_PACKET_SIZE;
        g_UsbDeviceVideoStreamEndpoints[0].maxPacketSize  = FS_STREAM_IN_PACKET_SIZE;

        g_UsbDeviceVideoControlEndpoints[0].interval = FS_INTERRUPT_IN_INTERVAL;
        g_UsbDeviceVideoStreamEndpoints[0].interval  = FS_STREAM_IN_INTERVAL;
    }

    return kStatus_USB_Success;
}
