/**
 * @file terminal.h
 * @brief Serial link with the radio: UART, AT command parsing and the
 *        central/peripheral connection state machines.
 *
 * `runUARTHead` is the main task: it reads bytes from the UART, detects AT
 * commands (delegating parsing to at_command.h), forwards telemetry/trainer
 * traffic between the radio and the BLE stack, and runs the state machines that
 * announce connections/disconnections to EdgeTX.
 */

#pragma once

/**
 * @brief Entry point of the UART task (created in main.c).
 * @param param  task parameter (unused).
 */
void runUARTHead(void *param);
