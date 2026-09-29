// wolfSSL's Arduino port reports internal traces through this hook
// (SecureClient enables it for TLS debugging); route it to the logger.
#if defined(FREEINK_NET_WOLFSSL)
#include <Logging.h>

extern "C" void wolfSSL_Arduino_Serial_Print(const char* const msg) { LOG_DBG("WOLFSSL", "%s", msg); }
#endif
