/**
 * @file at_command.c
 * @brief Implementation of at_command.h.
 */

#include "at_command.h"

#include <string.h>

at_command_parse_t at_command_parse(char *line)
{
  at_command_parse_t result = {.command = AT_CMD_UNKNOWN, .argument = ""};

  if (line == NULL) {
    return result;
  }

  // Strip the line terminator and trailing spaces (in place).
  size_t len = strlen(line);
  while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' ||
                     line[len - 1] == ' ' || line[len - 1] == '\t')) {
    line[--len] = '\0';
  }

  // Skip the optional "AT" prefix.
  const char *p = line;
  if (p[0] == 'A' && p[1] == 'T') {
    p += 2;
  }

  if (p[0] != '+') {
    return result;
  }

  if (strncmp(p, "+ROLE0", 6) == 0) {
    result.command = AT_CMD_ROLE0;
    result.argument = p + 6;
  } else if (strncmp(p, "+ROLE1", 6) == 0) {
    result.command = AT_CMD_ROLE1;
    result.argument = p + 6;
  } else if (strncmp(p, "+CON", 4) == 0) {
    result.command = AT_CMD_CON;
    result.argument = p + 4;
  } else if (strncmp(p, "+NAME", 5) == 0) {
    result.command = AT_CMD_NAME;
    result.argument = p + 5;
  } else if (strncmp(p, "+TXPW", 5) == 0) {
    result.command = AT_CMD_TXPW;
    result.argument = p + 5;
  } else if (strncmp(p, "+DISC?", 6) == 0) {
    result.command = AT_CMD_DISC;
    result.argument = "";
  } else if (strncmp(p, "+CLEAR", 6) == 0) {
    result.command = AT_CMD_CLEAR;
    result.argument = "";
  } else if (strncmp(p, "+BAUD", 5) == 0) {
    result.command = AT_CMD_BAUD;
    result.argument = p + 5;
  } else if (strncmp(p, "+HTRESET", 8) == 0) {
    result.command = AT_CMD_HTRESET;
    result.argument = "";
  }

  return result;
}
