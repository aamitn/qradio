/**
 ******************************************************************************
 * @author    bigwiz
 * @file    gsm.c
 * @brief   Platform-agnostic Quectel-style GSM/LTE AT-command driver.
 *          See gsm.h for integration instructions.
 ******************************************************************************
 */

#include "gsm.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* --- Internal state (private to this translation unit) --- */

static UART_HandleTypeDef *gsm_huart       = NULL;
static UART_HandleTypeDef *gsm_debug_huart = NULL;

static uint8_t          gsm_rx_buffer[GSM_RX_BUFFER_SIZE];
static volatile uint16_t gsm_write_pos = 0;

/* --- Internal helpers --- */

/**
 * @brief printf-style debug log, written directly to the attached debug
 *        UART via blocking HAL_UART_Transmit. Compiled out entirely when
 *        GSM_DEBUG_ENABLED is 0. Silently does nothing if no debug UART
 *        has been attached -- no dependency on the project's own printf.
 */
static void GSM_Debug(const char *fmt, ...)
{
#if GSM_DEBUG_ENABLED
    if (gsm_debug_huart == NULL) return;

    char buf[GSM_DEBUG_BUFFER_SIZE];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (len <= 0) return;
    if ((size_t)len >= sizeof(buf)) len = sizeof(buf) - 1; /* truncated */

    HAL_UART_Transmit(gsm_debug_huart, (uint8_t *)buf, (uint16_t)len, HAL_MAX_DELAY);
#else
    (void)fmt;
#endif
}

static void GSM_UART_StartReceive(void)
{
    if (gsm_huart == NULL) return;

    HAL_UART_AbortReceive(gsm_huart);   /* cleanly stop any in-flight reception */
    memset(gsm_rx_buffer, 0, GSM_RX_BUFFER_SIZE);
    gsm_write_pos = 0;

    HAL_UARTEx_ReceiveToIdle_DMA(gsm_huart, gsm_rx_buffer, GSM_RX_BUFFER_SIZE - 1);
    __HAL_DMA_DISABLE_IT(gsm_huart->hdmarx, DMA_IT_HT);  /* don't care about half-transfer */
}

/* --- Public: setup --- */

void GSM_AttachUart(UART_HandleTypeDef *huart)
{
    gsm_huart = huart;
}

void GSM_AttachDebugUart(UART_HandleTypeDef *huart)
{
    gsm_debug_huart = huart;
}

/* --- Public: HAL callback forwarding --- */

void GSM_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    if (gsm_huart == NULL || huart->Instance != gsm_huart->Instance) return;

    gsm_write_pos += Size;

    /* Re-arm to keep appending right after what we've received so far,
     * as long as there's still room left in the buffer. */
    if (gsm_write_pos < GSM_RX_BUFFER_SIZE - 1)
    {
        HAL_UARTEx_ReceiveToIdle_DMA(gsm_huart, gsm_rx_buffer + gsm_write_pos,
                                     GSM_RX_BUFFER_SIZE - 1 - gsm_write_pos);
        __HAL_DMA_DISABLE_IT(gsm_huart->hdmarx, DMA_IT_HT);
    }
}

void GSM_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (gsm_huart == NULL || huart->Instance != gsm_huart->Instance) return;

    HAL_UART_DMAStop(huart);
    memset(gsm_rx_buffer, 0, GSM_RX_BUFFER_SIZE);
    gsm_write_pos = 0;
    HAL_UARTEx_ReceiveToIdle_DMA(gsm_huart, gsm_rx_buffer, GSM_RX_BUFFER_SIZE - 1);
    __HAL_DMA_DISABLE_IT(gsm_huart->hdmarx, DMA_IT_HT);
}

/* --- Public: low-level command primitive --- */

HAL_StatusTypeDef GSM_SendATCommand(const char *cmd, const char *expected_resp, uint32_t timeout_ms)
{
    if (gsm_huart == NULL)
    {
        GSM_Debug("[ERR] GSM UART not attached -- call GSM_AttachUart() first\r\n");
        return HAL_ERROR;
    }

    GSM_UART_StartReceive();

    GSM_Debug("\r\n[TX] %s", cmd);
    HAL_UART_Transmit(gsm_huart, (uint8_t *)cmd, strlen(cmd), 1000);

    uint32_t start_time = HAL_GetTick();
    uint8_t match_found = 0;

    while ((HAL_GetTick() - start_time) < timeout_ms)
    {
        if (strstr((char *)gsm_rx_buffer, expected_resp) != NULL)
        {
            match_found = 1;
            break;
        }
    }

    GSM_Debug("\r\n[RX]\r\n%s\r\n", gsm_rx_buffer);

    if (match_found) return HAL_OK;

    GSM_Debug("[ERR] Expected response '%s' not found!\r\n", expected_resp);
    return HAL_TIMEOUT;
}

/* --- Public: modem lifecycle --- */

HAL_StatusTypeDef GSM_Init(uint8_t at_retry_count)
{
    if (gsm_huart == NULL)
    {
        GSM_Debug("[ERR] GSM UART not attached -- call GSM_AttachUart() before GSM_Init()\r\n");
        return HAL_ERROR;
    }

    /* 1. Check basic communication -- BOUNDED, so we always return control
     * to the caller even if the modem is disconnected/unpowered. */
    uint8_t at_ok = 0;
    for (uint8_t i = 0; i < at_retry_count; i++) {
        if (GSM_SendATCommand("AT\r\n", "OK", 1000) == HAL_OK) {
            at_ok = 1;
            break;
        }
        HAL_Delay(1000);
    }

    if (!at_ok) {
        GSM_Debug("[ERR] Modem did not respond to AT after %u attempts -- giving up\r\n", at_retry_count);
        return HAL_TIMEOUT;
    }

    /* 2. Check SIM Card Readiness */
    if (GSM_SendATCommand("AT+CPIN?\r\n", "+CPIN: READY", 2000) != HAL_OK) {
        return HAL_ERROR;
    }

    /* 3. Wait for Network Registration */
    uint8_t retries = 10;
    uint8_t registered = 0;
    while (retries--) {
        if (GSM_SendATCommand("AT+CREG?\r\n", "+CREG: 0,1", 2000) == HAL_OK ||
            GSM_SendATCommand("AT+CREG?\r\n", "+CREG: 0,5", 2000) == HAL_OK) {
            registered = 1;
            break;
        }
        HAL_Delay(2000);
    }

    if (!registered) {
        GSM_Debug("[ERR] Network registration failed\r\n");
        return HAL_ERROR;
    }

    /*
     * APN LIST INDIA:
		AT+QICSGP=1,1,"JIONET","","",0\r\n for JIO SIM
		AT+QICSGP=1,1,"airtelgprs.com","","",0\r\n for Airtel SIM
		AT+QICSGP=1,1,"portalnmms","","",0\r\n for Vodafone SIM
		AT+QICSGP=1,1,"bsnlnet","","",0\r\n for Airtel SIM
     */
    /* 4. Set APN Context */
    GSM_SendATCommand("AT+QICSGP=1,1,\"portalnmms\",\"\",\"\",0\r\n", "OK", 2000);

    /* 5. Activate Context */
    GSM_SendATCommand("AT+QIACT=1\r\n", "OK", 5000);

    /* --- 6: Configure HTTP Stack --- */
    GSM_SendATCommand("AT+QHTTPCFG=\"contextid\",1\r\n", "OK", 2000);
    GSM_SendATCommand("AT+QHTTPCFG=\"responseheader\",0\r\n", "OK", 2000);

    /* --- 7: Configure SSL Stack for HTTPS URLs --- */
    GSM_SendATCommand("AT+QHTTPCFG=\"sslctxid\",1\r\n", "OK", 2000);
    GSM_SendATCommand("AT+QSSLCFG=\"sslversion\",1,4\r\n", "OK", 2000);
    GSM_SendATCommand("AT+QSSLCFG=\"ciphersuite\",1,0xFFFF\r\n", "OK", 2000);
    /* Level 0: Ignore SSL certificate verification (crucial for easy testing) */
    GSM_SendATCommand("AT+QSSLCFG=\"seclevel\",1,0\r\n", "OK", 2000);
    /* Enable SNI (Server Name Indication) -- required for modern cloud APIs */
    GSM_SendATCommand("AT+QSSLCFG=\"sni\",1,1\r\n", "OK", 2000);

    return HAL_OK;
}

/* --- Public: SMS --- */

HAL_StatusTypeDef GSM_SendSMS_Multiple(const char *phone_numbers[], uint8_t num_contacts, const char *message)
{
    char cmd_buffer[64];
    HAL_StatusTypeDef final_status = HAL_OK;

    /* 1. Set SMS format to Text Mode ONCE for all messages */
    if (GSM_SendATCommand("AT+CMGF=1\r\n", "OK", 2000) != HAL_OK) {
        GSM_Debug("[ERR] Failed to set SMS Text Mode\r\n");
        return HAL_ERROR;
    }

    /* 2. Loop through the array of phone numbers */
    for (uint8_t i = 0; i < num_contacts; i++)
    {
        GSM_Debug("\r\n--- Sending SMS to %s ---\r\n", phone_numbers[i]);

        sprintf(cmd_buffer, "AT+CMGS=\"%s\"\r\n", phone_numbers[i]);

        /* Send command and wait for the '>' prompt (NOT "OK") */
        if (GSM_SendATCommand(cmd_buffer, ">", 3000) != HAL_OK) {
            GSM_Debug("[ERR] Did not receive '>' prompt for %s\r\n", phone_numbers[i]);
            GSM_SendATCommand("\x1B", "OK", 1000); /* ESC cancels the SMS operation if hung */
            final_status = HAL_ERROR;
            continue;
        }

        /* 3. Send the actual text message payload */
        GSM_Debug("\r\n[TX MSG] %s", message);
        HAL_UART_Transmit(gsm_huart, (uint8_t *)message, strlen(message), 1000);

        /* 4. Send Ctrl+Z (Hex 0x1A) to complete and transmit */
        if (GSM_SendATCommand("\x1A", "OK", 15000) != HAL_OK) {
            GSM_Debug("[ERR] Network failed to send SMS to %s\r\n", phone_numbers[i]);
            final_status = HAL_ERROR;
        }

        HAL_Delay(2000); /* short delay so the cellular network doesn't drop back-to-back messages */
    }

    return final_status;
}

/* --- Public: HTTP --- */

static HAL_StatusTypeDef GSM_HTTP_SetURL(const char *url)
{
    char cmd_buffer[64];
    sprintf(cmd_buffer, "AT+QHTTPURL=%d,5\r\n", (int)strlen(url));

    if (GSM_SendATCommand(cmd_buffer, "CONNECT\r\n", 5000) == HAL_OK) {
        HAL_Delay(100); /* give the modem a brief moment to enter raw data mode */
        return GSM_SendATCommand(url, "OK", 5000);
    }
    return HAL_ERROR;
}

HAL_StatusTypeDef GSM_HTTP_GET(const char *url)
{
    if (GSM_HTTP_SetURL(url) != HAL_OK) return HAL_ERROR;

    if (GSM_SendATCommand("AT+QHTTPGET=60\r\n", "+QHTTPGET:", 60000) == HAL_OK) {
        GSM_Debug("\r\n--- GET Request Finished, Reading Response ---\r\n");
        return GSM_SendATCommand("AT+QHTTPREAD=80\r\n", "+QHTTPREAD: 0", 10000);
    }

    GSM_Debug("\r\n[ERR] GET Request Failed\r\n");
    return HAL_ERROR;
}

HAL_StatusTypeDef GSM_HTTP_POST(const char *url, const char *payload, const char *content_type)
{
    if (GSM_HTTP_SetURL(url) != HAL_OK) return HAL_ERROR;

    char cmd_buffer[128];

    GSM_SendATCommand("AT+QHTTPCFG=\"contenttype\",1\r\n", "OK", 2000);

    sprintf(cmd_buffer, "AT+QHTTPCFG=\"custom_header\",\"Content-Type: %s\r\n\"\r\n", content_type);
    GSM_SendATCommand(cmd_buffer, "OK", 2000);

    sprintf(cmd_buffer, "AT+QHTTPPOST=%d,60,80\r\n", (int)strlen(payload));

    if (GSM_SendATCommand(cmd_buffer, "CONNECT\r\n", 5000) == HAL_OK) {
        HAL_Delay(100);

        if (GSM_SendATCommand(payload, "+QHTTPPOST:", 80000) == HAL_OK) {
            GSM_Debug("\r\n--- POST Request Finished, Reading Response ---\r\n");
            return GSM_SendATCommand("AT+QHTTPREAD=80\r\n", "+QHTTPREAD: 0", 10000);
        }
    }

    GSM_Debug("\r\n[ERR] POST Request Failed\r\n");
    return HAL_ERROR;
}

/* --- Public: Ping --- */

HAL_StatusTypeDef GSM_Ping(const char *host, uint32_t timeout_ms)
{
    if (gsm_huart == NULL)
    {
        GSM_Debug("[ERR] GSM UART not attached -- call GSM_AttachUart() first\r\n");
        return HAL_ERROR;
    }

    char cmd_buffer[64];
    sprintf(cmd_buffer, "AT+QPING=1,\"%s\"\r\n", host);

    GSM_UART_StartReceive();

    GSM_Debug("\r\n[TX] %s", cmd_buffer);
    HAL_UART_Transmit(gsm_huart, (uint8_t *)cmd_buffer, strlen(cmd_buffer), 1000);

    uint32_t start_time = HAL_GetTick();
    uint8_t summary_found = 0;

    while ((HAL_GetTick() - start_time) < timeout_ms)
    {
        /* Summary line looks like: +QPING: 0,4,4,0,60,115,69       (7 plain ints)
         * Per-packet lines look like: +QPING: 0,"1.2.3.4",64,60,255 (quoted IP)
         * Must check the LAST "+QPING: " occurrence in the buffer, since
         * per-packet lines arrive first and would otherwise always match
         * before the real summary line shows up. */
        char *p = NULL;
        char *search = (char *)gsm_rx_buffer;
        while ((search = strstr(search, "+QPING: ")) != NULL)
        {
            p = search;
            search += 8;
        }

        if (p != NULL)
        {
            char *line_end = strstr(p, "\r\n");
            if (line_end != NULL)
            {
                char saved = *line_end;
                *line_end = '\0';

                int a, b, c, d, e, f, g;
                int matched = sscanf(p, "+QPING: %d,%d,%d,%d,%d,%d,%d",
                                     &a, &b, &c, &d, &e, &f, &g);

                *line_end = saved;

                if (matched == 7)
                {
                    summary_found = 1;
                    break;
                }
            }
        }
    }

    GSM_Debug("\r\n[RX]\r\n%s\r\n", gsm_rx_buffer);

    if (summary_found) return HAL_OK;

    GSM_Debug("[ERR] Ping summary not received!\r\n");
    return HAL_TIMEOUT;
}

/* --- Public: network status queries --- */

HAL_StatusTypeDef GSM_GetSignalQuality(int *rssi_dbm, uint8_t *ber)
{
    if (GSM_SendATCommand("AT+CSQ\r\n", "+CSQ:", 2000) != HAL_OK) {
        return HAL_ERROR;
    }

    char *p = strstr((char *)gsm_rx_buffer, "+CSQ:");
    if (p == NULL) return HAL_ERROR;

    int rssi_raw, ber_raw;
    if (sscanf(p, "+CSQ: %d,%d", &rssi_raw, &ber_raw) != 2) {
        return HAL_ERROR;
    }

    if (rssi_raw == 99) {
        /* 99 = "not known or not detectable" per the AT command spec */
        if (rssi_dbm) *rssi_dbm = 0;
        if (ber)      *ber = 99;
        return HAL_ERROR;
    }

    if (rssi_dbm) *rssi_dbm = -113 + (rssi_raw * 2);
    if (ber)      *ber = (uint8_t)ber_raw;

    return HAL_OK;
}

HAL_StatusTypeDef GSM_GetRegistrationStatus(GSM_RegStatus_t *status)
{
    if (status == NULL) return HAL_ERROR;

    /* AT+CEREG? (EPS registration) is the correct query for LTE Cat 1 --
     * AT+CREG? is the legacy 2G/3G circuit-switched equivalent and won't
     * reliably reflect LTE attach state on this modem. */
    if (GSM_SendATCommand("AT+CEREG?\r\n", "+CEREG:", 2000) != HAL_OK) {
        return HAL_ERROR;
    }

    char *p = strstr((char *)gsm_rx_buffer, "+CEREG:");
    if (p == NULL) return HAL_ERROR;

    int n, stat;
    if (sscanf(p, "+CEREG: %d,%d", &n, &stat) != 2) {
        return HAL_ERROR;
    }

    *status = (GSM_RegStatus_t)stat;
    return HAL_OK;
}

HAL_StatusTypeDef GSM_GetNetworkName(char *name_buf, size_t buf_size)
{
    if (name_buf == NULL || buf_size == 0) return HAL_ERROR;
    name_buf[0] = '\0';

    if (GSM_SendATCommand("AT+COPS?\r\n", "+COPS:", 3000) != HAL_OK) {
        return HAL_ERROR;
    }

    char *p = strstr((char *)gsm_rx_buffer, "+COPS:");
    if (p == NULL) return HAL_ERROR;

    /* Operator name is the quoted field: +COPS: <mode>,<format>,"<name>",<AcT>
     * If not currently registered, the quoted field is often absent entirely. */
    char *quote_start = strchr(p, '"');
    if (quote_start == NULL) return HAL_ERROR;
    quote_start++;

    char *quote_end = strchr(quote_start, '"');
    if (quote_end == NULL) return HAL_ERROR;

    size_t len = (size_t)(quote_end - quote_start);
    if (len >= buf_size) len = buf_size - 1;

    memcpy(name_buf, quote_start, len);
    name_buf[len] = '\0';

    return HAL_OK;
}

HAL_StatusTypeDef GSM_GetNetworkStatus(GSM_NetworkStatus_t *out)
{
    if (out == NULL) return HAL_ERROR;

    memset(out, 0, sizeof(*out));
    uint8_t all_ok = 1;

    if (GSM_GetSignalQuality(&out->rssi_dbm, &out->ber) == HAL_OK) {
        out->rssi_valid = 1;
    } else {
        all_ok = 0;
    }

    if (GSM_GetRegistrationStatus(&out->reg_status) == HAL_OK) {
        out->reg_valid = 1;
    } else {
        all_ok = 0;
    }

    if (GSM_GetNetworkName(out->operator_name, sizeof(out->operator_name)) == HAL_OK) {
        out->operator_valid = 1;
    } else {
        all_ok = 0;
    }

    return all_ok ? HAL_OK : HAL_ERROR;
}

static const char *GSM_RegStatusToString(GSM_RegStatus_t status)
{
    switch (status) {
        case GSM_REG_NOT_REGISTERED: return "Not registered";
        case GSM_REG_HOME:           return "Registered (home)";
        case GSM_REG_SEARCHING:      return "Searching...";
        case GSM_REG_DENIED:         return "Registration denied";
        case GSM_REG_UNKNOWN:        return "Unknown / out of coverage";
        case GSM_REG_ROAMING:        return "Registered (roaming)";
        default:                     return "Invalid status";
    }
}

void GSM_PrintNetworkStatus(void)
{
    GSM_NetworkStatus_t status;
    GSM_GetNetworkStatus(&status); /* fields default to invalid/zeroed on partial failure */

    GSM_Debug("\r\n--- Network Status ---\r\n");

    if (status.rssi_valid) {
        GSM_Debug("Signal: %d dBm (BER index %u)\r\n", status.rssi_dbm, status.ber);
    } else {
        GSM_Debug("Signal: unknown\r\n");
    }

    if (status.reg_valid) {
        GSM_Debug("Registration: %s\r\n", GSM_RegStatusToString(status.reg_status));
    } else {
        GSM_Debug("Registration: query failed\r\n");
    }

    if (status.operator_valid) {
        GSM_Debug("Operator: %s\r\n", status.operator_name);
    } else {
        GSM_Debug("Operator: unavailable\r\n");
    }

    GSM_Debug("----------------------\r\n");
}
