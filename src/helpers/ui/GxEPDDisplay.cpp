
#include "GxEPDDisplay.h"

#ifdef EXP_PIN_BACKLIGHT
  #include <PCA9557.h>
  extern PCA9557 expander;
#endif

#ifndef DISPLAY_ROTATION
  #define DISPLAY_ROTATION 3
#endif

#ifdef ESP32
  SPIClass SPI1 = SPIClass(FSPI);
#endif

// Color scheme
ColorVal UIColor::window_bkg = GxEPD_WHITE;
ColorVal UIColor::title_bkg = GxEPD_WHITE;
ColorVal UIColor::title_txt = GxEPD_BLACK;
ColorVal UIColor::primary_txt = GxEPD_BLACK;
ColorVal UIColor::secondary_txt = GxEPD_BLACK;
ColorVal UIColor::warning_txt = GxEPD_BLACK;
ColorVal UIColor::popup_bkg = GxEPD_WHITE;
ColorVal UIColor::popup_txt = GxEPD_BLACK;
ColorVal UIColor::corp_blue = GxEPD_BLACK;


bool GxEPDDisplay::begin() {
  display.epd2.selectSPI(SPI1, SPISettings(4000000, MSBFIRST, SPI_MODE0));
#ifdef ESP32
  SPI1.begin(PIN_DISPLAY_SCLK, PIN_DISPLAY_MISO, PIN_DISPLAY_MOSI, PIN_DISPLAY_CS);
#else
  SPI1.begin();
#endif
  display.init(115200, true, 2, false);
  display.setRotation(DISPLAY_ROTATION);
  // Adafruit_GFX wraps text at the right edge by itself, which this UI does not want: it lays
  // its own lines out, so an over-long line was wrapped back to x=0 one font-height down, on top
  // of whatever was drawn on the next row. Worse, getTextBounds() honours the same flag, so a
  // long string measured as no wider than the screen and callers doing their own wrapping (the
  // message reader) never saw a line that needed breaking. Measure and draw unwrapped instead.
  display.setTextWrap(false);
  setTextSize(1);  // Default to size 1
  display.setPartialWindow(0, 0, display.width(), display.height());

  display.fillScreen(GxEPD_WHITE);
  display.display(true);
  #if DISP_BACKLIGHT
  digitalWrite(DISP_BACKLIGHT, LOW);
  pinMode(DISP_BACKLIGHT, OUTPUT);
  #endif
  _init = true;
  return true;
}

void GxEPDDisplay::turnOn() {
  if (!_init) begin();
#if defined(DISP_BACKLIGHT) && !defined(BACKLIGHT_BTN)
  digitalWrite(DISP_BACKLIGHT, HIGH);
#elif defined(EXP_PIN_BACKLIGHT) && !defined(BACKLIGHT_BTN)
  expander.digitalWrite(EXP_PIN_BACKLIGHT, HIGH);
#endif
  _isOn = true;
}

void GxEPDDisplay::turnOff() {
#if defined(DISP_BACKLIGHT) && !defined(BACKLIGHT_BTN)
  digitalWrite(DISP_BACKLIGHT, LOW);
#elif defined(EXP_PIN_BACKLIGHT) && !defined(BACKLIGHT_BTN)
  expander.digitalWrite(EXP_PIN_BACKLIGHT, LOW);
#endif
  _isOn = false;
}

void GxEPDDisplay::clear() {
  display.fillScreen(GxEPD_WHITE);
  display.setTextColor(GxEPD_BLACK);
  display_crc.reset();
}

void GxEPDDisplay::startFrame(ColorVal bkg) {
  display.fillScreen(bkg);
  display.setTextColor(_curr_color = UIColor::primary_txt);
  display_crc.reset();
}

void GxEPDDisplay::setTextSize(int sz) {
  display_crc.update<int>(sz);
  const GFXfont* font;
  switch(sz) {
    case 1:  // Small
      font = &FreeSans9pt7b;
      break;
    case 2:  // Medium Bold
      font = &FreeSansBold12pt7b;
      break;
    case 3:  // Large
      font = &FreeSans18pt7b;
      break;
    default:
      font = &FreeSans9pt7b;
      break;
  }
  display.setFont(font);
  // kept for printWordWrap(): the font is only reachable through Adafruit_GFX's protected
  // members, so note its line spacing here, where the font is chosen
  _line_height = pgm_read_byte(&font->yAdvance);
}

void GxEPDDisplay::setColor(ColorVal c) {
  display_crc.update<ColorVal> (c);
  display.setTextColor(_curr_color = c);
}

void GxEPDDisplay::setCursor(int x, int y) {
  display_crc.update<int>(x);
  display_crc.update<int>(y);
  _cursor_x = x;
  _cursor_y = y;
  display.setCursor((x+offset_x)*scale_x, (y+offset_y)*scale_y);
}

void GxEPDDisplay::print(const char* str) {
  display_crc.update<char>(str, strlen(str));
  display.print(str);
}

// Print str broken into lines no wider than max_width, starting at the current cursor. Breaks at
// the last space that fits, or mid-word when a single word is too long for a line on its own.
// Needed because Adafruit_GFX's own wrapping is switched off in begin(), and because that
// wrapping went by the physical screen edge rather than the width the caller asked for.
void GxEPDDisplay::printWordWrap(const char* str, int max_width) {
  char line[96];
  int x = _cursor_x, y = _cursor_y;
  int avail = max_width - x;
  if (avail < 1) return;

  int line_h = (int)ceil(_line_height / scale_y);
  for (const char* p = str; *p != 0 && y < height(); ) {
    size_t fits = 0, last_space = 0;
    for (size_t n = 1; n < sizeof(line) && p[n - 1] != 0; n++) {
      memcpy(line, p, n);
      line[n] = 0;
      if ((int)getTextWidth(line) > avail) break;
      fits = n;
      if (p[n - 1] == ' ') last_space = n;
    }
    // take the whole prefix that fits, unless stopping there would split a word
    size_t take = (p[fits] != 0 && last_space > 0) ? last_space : (fits > 0 ? fits : 1);

    memcpy(line, p, take);
    line[take] = 0;
    setCursor(x, y);
    print(line);

    p += take;
    while (*p == ' ') p++;   // don't start the next line on the break space
    y += line_h;
  }
}

void GxEPDDisplay::fillRect(int x, int y, int w, int h) {
  display_crc.update<int>(x);
  display_crc.update<int>(y);
  display_crc.update<int>(w);
  display_crc.update<int>(h);
  display.fillRect(x*scale_x, y*scale_y, w*scale_x, h*scale_y, _curr_color);
}

void GxEPDDisplay::drawRect(int x, int y, int w, int h) {
  display_crc.update<int>(x);
  display_crc.update<int>(y);
  display_crc.update<int>(w);
  display_crc.update<int>(h);
  display.drawRect(x*scale_x, y*scale_y, w*scale_x, h*scale_y, _curr_color);
}

void GxEPDDisplay::drawXbm(int x, int y, const uint8_t* bits, int w, int h) {
  display_crc.update<int>(x);
  display_crc.update<int>(y);
  display_crc.update<int>(w);
  display_crc.update<int>(h);
  display_crc.update<uint8_t>(bits, w * h / 8);
  // Calculate the base position in display coordinates
  uint16_t startX = x * scale_x;
  uint16_t startY = y * scale_y;
  
  // Width in bytes for bitmap processing
  uint16_t widthInBytes = (w + 7) / 8;
  
  // Process the bitmap row by row
  for (uint16_t by = 0; by < h; by++) {
    // Calculate the target y-coordinates for this logical row
    int y1 = startY + (int)(by * scale_y);
    int y2 = startY + (int)((by + 1) * scale_y);
    int block_h = y2 - y1;
    
    // Scan across the row bit by bit
    for (uint16_t bx = 0; bx < w; bx++) {
      // Calculate the target x-coordinates for this logical column
      int x1 = startX + (int)(bx * scale_x);
      int x2 = startX + (int)((bx + 1) * scale_x);
      int block_w = x2 - x1;
      
      // Get the current bit
      uint16_t byteOffset = (by * widthInBytes) + (bx / 8);
      uint8_t bitMask = 0x80 >> (bx & 7);
      bool bitSet = pgm_read_byte(bits + byteOffset) & bitMask;
      
      // If the bit is set, draw a block of pixels
      if (bitSet) {
        // Draw the block as a filled rectangle
        display.fillRect(x1, y1, block_w, block_h, _curr_color);
      }
    }
  }
}

uint16_t GxEPDDisplay::getTextWidth(const char* str) {
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(str, 0, 0, &x1, &y1, &w, &h);
  return ceil((w + 1) / scale_x);
}

void GxEPDDisplay::endFrame() {
  uint32_t crc = display_crc.finalize();
  if (crc != last_display_crc_value) {
    display.display(true);
    last_display_crc_value = crc;
  }
}
