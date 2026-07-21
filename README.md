# Quectel GSM/LTE AT-Command Driver

A robust, platform-agnostic C driver for Quectel-style GSM/LTE cellular modules. This library provides a high-level API for HTTP/HTTPS (with modern TLS/SNI support), SMS, and network diagnostics, built on a non-blocking DMA UART architecture.

Currently deployed and tested on the **STM32F411 "Blackpill"** platform, and **ECU200U-CN Module from 7Semi** but designed to be easily portable to any MCU family and ECU200 device.

Example CubeIDE+CubeMX Project is in `bp_testmod` folder.

## ✨ Features

* **Zero Busy-Wait Reception:** Uses DMA + IDLE-line reception (`HAL_UARTEx_ReceiveToIdle_DMA`). The CPU is completely free while waiting for modem responses.
* **Modern Web Ready:** Full support for HTTPS, TLS 1.2, and SNI (Server Name Indication) required by modern cloud APIs (e.g., AWS, Beeceptor, custom backends).
* **High-Level API:** Simple wrappers for `GET`, `POST` (JSON/Form), `PING`, and multi-contact SMS.
* **Robust Network Diagnostics:** Easy querying of Signal Strength (RSSI/BER), EPS/LTE Registration Status, and Operator Name.
* **Decoupled Debugging:** Internal debug logging (`[TX]`, `[RX]`, `[ERR]`) runs on a separate, optional UART and does not hijack your global `printf`/`_write` syscalls.

## 📋 Prerequisites (STM32CubeMX Setup)

To allow the non-blocking architecture to work, you **must** configure your UART peripherals with DMA and global interrupts in your `.ioc` file:

### 1. Modem UART (e.g., USART1)
* **NVIC Settings:** Check the box to enable the **USARTx global interrupt** (required for IDLE line detection).
* **DMA Settings:** Add a DMA request for **USARTx_RX**.
  * Mode: `Normal`
  * Data Width: `Byte` / `Byte`
* *(Optional)* Add a DMA request for **USARTx_TX** (Mode: `Normal`).

### 2. Debug UART (e.g., USART2)
* **DMA Settings:** Add a DMA request for **USARTx_TX**.
  * Mode: `Normal`
  * Data Width: `Byte` / `Byte`
  * *Note: Adding DMA for the debug console ensures smooth, high-speed log output without holding up your main application loop.*
  

## 🚀 Integration Guide

Integrating this driver into an STM32 HAL project requires just three steps.

### 1. Include the Header
Add the driver to your `main.c` (or wherever your network logic lives):
```c
#include "gsm.h"
```
### 2. Attach the UART Peripherals

Before initializing the modem, tell the driver which UART is connected to the GSM module, and optionally which UART to use for debug logs.
```C

/* Attach modem UART (e.g., huart1) */
GSM_AttachUart(&huart1);

/* (Optional) Attach debug console UART (e.g., huart2) */
GSM_AttachDebugUart(&huart2);
```
### 3. Forward the HAL Callbacks

The driver relies on DMA IDLE line detection to know when the modem has finished speaking. Forward the global HAL UART callbacks from main.c into the driver:
```C

/* --- DMA + IDLE-line event-driven reception --- */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    GSM_RxEventCallback(huart, Size);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    GSM_ErrorCallback(huart);
}
```

## 💻 Example Usage

Here is a complete example of initializing the modem, checking the network status, and sending an SMS and HTTPS GET + POST request.
```C

// 1. Initialize the modem (allows up to 5 retries if the modem is booting)
HAL_StatusTypeDef status = GSM_Init(5);

if (status == HAL_OK) {
    // 2. Print Network Status (Signal, Operator, Registration) to debug serial
    GSM_PrintNetworkStatus();

    // 3. Send an SMS to multiple contacts
    const char* target_contacts[] = {"+919000000000", "+918000000000"};
    GSM_SendSMS_Multiple(target_contacts, 2, "STM32 System Online!");
	
	 // 4. Perform an HTTPS GET request with JSON
	 GSM_HTTP_GET("https://test-api.free.beeceptor.com/");

    // 4.1. Perform an HTTPS POST request with JSON
    const char* my_json = "{\"device\":\"stm32_blackpill\",\"status\":\"active\"}";
    GSM_HTTP_POST("[https://your-api.com/endpoint](https://your-api.com/endpoint)", my_json, "application/json");

    // 5. Ping Google
    GSM_Ping("[www.google.com](https://www.google.com)", 15000);
}
```

## ⚙️ Configuration

You can override the default buffer sizes and debug behavior by setting these macros in your build environment (or at the top of gsm.h):

    - **GSM_RX_BUFFER_SIZE (Default: 512)**: Adjust based on the largest expected HTTP response chunk.
    - **GSM_DEBUG_ENABLED (Default: 1)**: Set to 0 to completely strip out debug strings and HAL_UART_Transmit blocking calls for production firmware.
    - **GSM_DEBUG_BUFFER_SIZE (Default: 600)**: Maximum length of a single debug log line.

## 📡 Supported APNs

The GSM_Init() sequence currently configures the PDP context. If you are modifying this for different regions, update the AT+QICSGP command in gsm.c:

    - Vodafone (India): portalnmms
    - Jio: JIONET
    - Airtel: airtelgprs.com
    - BSNL: bsnlnet

Maintained by Amit| *Bitmutex Technologies*