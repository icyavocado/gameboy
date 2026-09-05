#include "ui_assets.h"
#include <stdio.h>
#include <string.h>

static int icon_index(const char *name) {
  static const char *const names[] = {"arrow_up", "arrow_down", "arrow_left",
                                      "arrow_right"};
  for (int i = 0; i < 4; i++)
    if (strcmp(name, names[i]) == 0)
      return i;
  return -1;
}

static char *trim(char *line) {
  char *end;
  while (*line == ' ' || *line == '\t')
    line++;
  end = line + strlen(line);
  while (end > line && (end[-1] == '\n' || end[-1] == '\r' ||
                        end[-1] == ' ' || end[-1] == '\t'))
    *--end = '\0';
  return line;
}

int gb_ui_assets_load(gb_ui_assets_t *assets, const char *path) {
  FILE *file = fopen(path, "r");
  char line[128];
  int type = 0, index = 0, row = 0;
  if (!file)
    return -1;
  memset(assets, 0, sizeof *assets);
  while (fgets(line, sizeof line, file)) {
    char *text = trim(line);
    if (!*text || *text == ';')
      continue;
    if (strncmp(text, "font ", 5) == 0) {
      index = text[5] == '_' ? ' ' : (unsigned char)text[5];
      type = 1;
      row = 0;
      continue;
    }
    if (strncmp(text, "icon ", 5) == 0) {
      if (strcmp(text + 5, "cog") == 0) {
        type = 2;
        index = 0;
      } else if (strcmp(text + 5, "bug") == 0) {
        type = 3;
        index = 0;
      } else {
        index = icon_index(text + 5);
        type = index >= 0 ? 4 : 0;
      }
      row = 0;
      continue;
    }
    if (type == 1 && index >= 0 && index < 128 && row < 7) {
      for (int bit = 0; bit < 5 && text[bit]; bit++)
        if (text[bit] == '#' || text[bit] == '1')
          assets->font[index][row] |= (uint8_t)(1u << (4 - bit));
      row++;
    } else if (type == 2 && row < 7) {
      for (int bit = 0; bit < 7 && text[bit]; bit++)
        if (text[bit] == '#' || text[bit] == '1')
          assets->cog[row] |= (uint8_t)(1u << (6 - bit));
      row++;
    } else if (type == 3 && row < 7) {
      for (int bit = 0; bit < 7 && text[bit]; bit++)
        if (text[bit] == '#' || text[bit] == '1')
          assets->bug[row] |= (uint8_t)(1u << (6 - bit));
      row++;
    } else if (type == 4 && index >= 0 && row < 7) {
      for (int bit = 0; bit < 7 && text[bit]; bit++)
        if (text[bit] == '#' || text[bit] == '1')
          assets->arrow[index][row] |= (uint8_t)(1u << (6 - bit));
      row++;
    }
  }
  fclose(file);
  return 0;
}
