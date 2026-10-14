/**
 * @file at_command.h
 * @brief Pure parser for the EdgeTX AT command set (no ESP-IDF dependencies,
 *        host-testable).
 *
 * EdgeTX talks to the Bluetooth module through `AT+CMD` commands terminated by
 * `\r\n`. This module converts a line into a structured command and its
 * argument, leaving the *response* and the *action* to the upper layer
 * (terminal.c).
 *
 * See `doc/AUDIT.md` for the full protocol contract.
 */

#pragma once

/** Recognized AT commands (`AT+` prefix omitted). */
typedef enum {
  AT_CMD_UNKNOWN = 0,  //< unrecognized line
  AT_CMD_ROLE0,        //< AT+ROLE0  -> peripheral role
  AT_CMD_ROLE1,        //< AT+ROLE1  -> central role
  AT_CMD_CON,          //< AT+CON<addr> -> connect to <addr>
  AT_CMD_NAME,         //< AT+NAME<name> -> set name
  AT_CMD_TXPW,         //< AT+TXPW<n> -> TX power
  AT_CMD_DISC,         //< AT+DISC?  -> start discovery
  AT_CMD_CLEAR,        //< AT+CLEAR  -> forget saved device
  AT_CMD_BAUD,         //< AT+BAUD<n> -> baud rate level
  AT_CMD_HTRESET,      //< AT+HTRESET -> reset headtracker (project extension)
} at_command_t;

/** Result of parsing an AT line. */
typedef struct {
  at_command_t command;  //< recognized command (or AT_CMD_UNKNOWN)
  const char *argument;  //< pointer to the argument within `line` ("" if none)
} at_command_parse_t;

/**
 * @brief Parse an AT command line.
 *
 * Accepts both `AT+CMD...` and `+CMD...` forms, and strips trailing
 * `\r`/`\n`/space characters (modifying the buffer in place).
 *
 * @param line  NUL-terminated line received. Modified to strip the line
 *              terminator.
 * @return result with the command and argument. `argument` points into `line`.
 */
at_command_parse_t at_command_parse(char *line);
