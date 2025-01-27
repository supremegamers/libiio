#ifndef IIO_CONFIG_H
#define IIO_CONFIG_H

#define LIBIIO_VERSION_MAJOR	0
#define LIBIIO_VERSION_MINOR	24
#define LIBIIO_VERSION_GIT	"c4498c27"

#define LOG_LEVEL Info_L

#define LIBIIO_SCAN_BACKENDS	"local"

#define WITH_LOCAL_BACKEND 1
#define WITH_XML_BACKEND 0
#define WITH_NETWORK_BACKEND 0
#define WITH_USB_BACKEND 0
#define WITH_SERIAL_BACKEND 0

/* #undef WITH_NETWORK_GET_BUFFER */
#define WITH_NETWORK_EVENTFD 0
#define WITH_IIOD_USBD 0
#define WITH_IIOD_SERIAL 0
#define WITH_LOCAL_CONFIG 0
#define WITH_LOCAL_MMAP_API 0
#define WITH_HWMON 0
#define WITH_AIO 0
#define HAVE_DNS_SD 0
#define HAVE_AVAHI 0
#define WITH_ZSTD 0

/* #undef HAS_PIPE2 */
#define HAS_STRDUP
#define HAS_STRNDUP
#define HAS_STRTOK_R
#define HAS_STRERROR_R
#define HAS_NEWLOCALE
#define HAS_PTHREAD_SETNAME_NP
/* #undef HAVE_IPV6 */
/* #undef NO_THREADS */
/* #undef HAS_LIBUSB_GETVERSION */

#endif /* IIO_CONFIG_H */
