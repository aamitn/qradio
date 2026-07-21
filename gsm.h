/**
 ******************************************************************************
 * @author  bigwiz
 * @file    gsm.h
 * @brief   Platform-agnostic Quectel-style GSM/LTE AT-command driver.
 *
 *          Uses DMA + IDLE-line reception (HAL_UARTEx_ReceiveToIdle_DMA) on
 *          whichever UART is attached via GSM_AttachUart(). No busy-wait
 *          byte polling — the CPU is free while waiting on modem responses.
 *
 *          Integration steps in the consuming project:
 *            1. #include "gsm.h" in main.c
 *            2. Call GSM_AttachUart(&huartX); once, before GSM_Init();
 *            3. Forward the two HAL callbacks from main.c into this driver:
 *
 *               void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
 *               {
 *                   GSM_RxEventCallback(huart, Size);
 *               }
 *
 *               void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
 *               {
 *                   GSM_ErrorCallback(huart);
 *               }
 *
 *          This driver never touches USART2/printf/debug output directly;
 *          it just calls printf(), so route printf to whatever debug UART
 *          your project already uses (via _write(), as in the STM32CubeIDE
 *          "syscalls" pattern).
 ******************************************************************************
 */

#ifndef GSM_H
#define GSM_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"   /* pulls in UART_HandleTypeDef / HAL_StatusTypeDef for the target MCU family */
#include <stdint.h>

/* Override in your project (e.g. via a build define) if you need a bigger
 * or smaller RX buffer than the default. Must be big enough to hold the
 * largest single AT response you expect (HTTP read chunks, multi-packet
 * ping summaries, etc.). */
#ifndef GSM_RX_BUFFER_SIZE
#define GSM_RX_BUFFER_SIZE 512
#endif

/* Set to 0 (via a build define) to strip all [TX]/[RX]/[ERR] debug logging
 * out of the build entirely -- e.g. for a production image where you don't
 * want the blocking HAL_UART_Transmit debug calls or the extra flash. */
#ifndef GSM_DEBUG_ENABLED
#define GSM_DEBUG_ENABLED 1
#endif

/* Max length of a single formatted debug line (truncated beyond this). */
#ifndef GSM_DEBUG_BUFFER_SIZE
#define GSM_DEBUG_BUFFER_SIZE 600
#endif

/* --- Setup --- */

/**
 * @brief Attach the UART peripheral the modem is wired to.
 *        Must be called once, before any other GSM_* function.
 */
void GSM_AttachUart(UART_HandleTypeDef *huart);

/**
 * @brief Optionally attach a UART for this driver's own debug logging
 *        ([TX]/[RX]/[ERR] lines). Fully decoupled from the modem UART and
 *        from any project-level printf() redirection.
 *
 *        If never called (or called with NULL), debug logging is silently
 *        skipped -- the driver has no dependency on printf/_write being
 *        set up in the consuming project.
 */
void GSM_AttachDebugUart(UART_HandleTypeDef *huart);

/* --- HAL callback forwarding (call these from main.c's global HAL callbacks) --- */
void GSM_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size);
void GSM_ErrorCallback(UART_HandleTypeDef *huart);

/* --- Modem lifecycle --- */

/**
 * @brief Attempts modem bring-up: basic AT check, SIM ready, network
 *        registration, then APN/HTTP/SSL stack configuration.
 *
 *        Bounded -- always returns, even if the modem never responds
 *        (e.g. disconnected or unpowered), so the caller's main loop is
 *        guaranteed to keep running rather than hang forever in here.
 *
 * @param at_retry_count Max number of "AT" liveness checks to attempt
 *                        (1s apart) before giving up. Use a small number
 *                        (e.g. 5) to fail fast during bring-up/testing.
 * @return HAL_OK if the modem responded to AT and full init completed;
 *         HAL_TIMEOUT if the modem never responded to AT at all;
 *         HAL_ERROR if AT succeeded but SIM/registration failed.
 */
HAL_StatusTypeDef GSM_Init(uint8_t at_retry_count);

/* --- Low-level command primitive (used internally, but exposed for
 *     custom AT commands not covered by the helpers below) --- */

/**
 * @brief Send a raw AT command and wait for an expected substring in the
 *        response, up to timeout_ms. Returns HAL_OK if found, HAL_TIMEOUT
 *        otherwise. Not suitable for AT+QPING (use GSM_Ping instead), since
 *        QPING replies OK immediately then streams results asynchronously.
 */
HAL_StatusTypeDef GSM_SendATCommand(const char *cmd, const char *expected_resp, uint32_t timeout_ms);

/* --- High-level helpers --- */

HAL_StatusTypeDef GSM_SendSMS_Multiple(const char *phone_numbers[], uint8_t num_contacts, const char *message);
HAL_StatusTypeDef GSM_HTTP_GET(const char *url);
HAL_StatusTypeDef GSM_HTTP_POST(const char *url, const char *payload, const char *content_type);

/**
 * @brief Sends AT+QPING and waits for the final numeric summary line
 *        (+QPING: err,sent,recv,lost,min,max,avg), ignoring the earlier
 *        per-packet lines which contain a quoted IP address.
 */
HAL_StatusTypeDef GSM_Ping(const char *host, uint32_t timeout_ms);

/* --- Network status queries --- */

/** EPS (LTE) network registration status, per 3GPP AT+CEREG values. */
typedef enum {
    GSM_REG_NOT_REGISTERED = 0, /* not registered, not currently searching */
    GSM_REG_HOME           = 1, /* registered, home network */
    GSM_REG_SEARCHING      = 2, /* not registered, currently searching */
    GSM_REG_DENIED         = 3, /* registration denied */
    GSM_REG_UNKNOWN        = 4, /* unknown (e.g. out of coverage) */
    GSM_REG_ROAMING        = 5  /* registered, roaming */
} GSM_RegStatus_t;

/**
 * @brief Query signal quality via AT+CSQ.
 * @param rssi_dbm  Out: signal strength in dBm (typically -113 to -51).
 *                  Pass NULL if not needed.
 * @param ber       Out: bit error rate indicator, 0-7, or 99 if unknown.
 *                  Pass NULL if not needed.
 * @return HAL_OK if a valid reading was obtained; HAL_ERROR if the modem
 *         reported "unknown" (99) or the command/parse failed.
 */
HAL_StatusTypeDef GSM_GetSignalQuality(int *rssi_dbm, uint8_t *ber);

/**
 * @brief Query EPS/LTE network registration status via AT+CEREG?.
 * @param status Out: current registration status. Must not be NULL.
 * @return HAL_OK if the status was read successfully (check the value
 *         itself to see if it means "actually registered"); HAL_ERROR
 *         on command/parse failure.
 */
HAL_StatusTypeDef GSM_GetRegistrationStatus(GSM_RegStatus_t *status);

/**
 * @brief Query the currently registered network operator name via
 *        AT+COPS?.
 * @param name_buf  Destination buffer for the operator name (e.g. "Airtel").
 * @param buf_size  Size of name_buf, including space for the null terminator.
 * @return HAL_OK if a name was found and copied; HAL_ERROR if not currently
 *         registered to any operator, or on command/parse failure.
 */
HAL_StatusTypeDef GSM_GetNetworkName(char *name_buf, size_t buf_size);

/**
 * @brief Snapshot of all network status fields in one struct, for
 *        programmatic use (e.g. logging to a data channel, driving a
 *        signal-bar UI, deciding whether to attempt an HTTP request).
 *
 *        Each field has a paired *_valid flag, since any individual query
 *        can fail independently -- e.g. signal quality may read fine while
 *        registration is still in progress, or the operator name may be
 *        unavailable pre-registration even though CSQ/CEREG succeeded.
 */
typedef struct {
    int             rssi_dbm;        /* valid only if rssi_valid */
    uint8_t         ber;             /* valid only if rssi_valid */
    uint8_t         rssi_valid;

    GSM_RegStatus_t reg_status;      /* valid only if reg_valid */
    uint8_t         reg_valid;

    char            operator_name[32]; /* valid only if operator_valid; empty string otherwise */
    uint8_t         operator_valid;
} GSM_NetworkStatus_t;

/**
 * @brief Populates a GSM_NetworkStatus_t by running all three underlying
 *        queries (AT+CSQ, AT+CEREG?, AT+COPS?) in sequence.
 * @param out Destination struct. Must not be NULL. Always fully written --
 *            fields for any failed query are zeroed with their *_valid
 *            flag cleared, rather than left uninitialized.
 * @return HAL_OK if ALL three queries succeeded; HAL_ERROR if one or more
 *         failed (check the individual *_valid flags to see which).
 */
HAL_StatusTypeDef GSM_GetNetworkStatus(GSM_NetworkStatus_t *out);

/**
 * @brief Convenience helper: queries all three of the above and logs them
 *        via the attached debug UART in one readable block. Safe to call
 *        even if some queries fail (missing data is shown as such).
 */
void GSM_PrintNetworkStatus(void);

#ifdef __cplusplus
}
#endif

#endif /* GSM_H */
