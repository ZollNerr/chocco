// Solarman V5 протокол (Wi-Fi логгер LSW-3/LSW-5, TCP-порт 8899) + Modbus RTU.
// Формат кадров сверен с библиотекой pysolarmanv5:
// https://github.com/jmccrohan/pysolarmanv5
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace v5 {

// Modbus CRC16 (полином 0xA001)
inline uint16_t crc16(const uint8_t* d, size_t n) {
  uint16_t c = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int b = 0; b < 8; b++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0xA001) : (uint16_t)(c >> 1);
  }
  return c;
}

const size_t REQ_LEN = 36;        // 11 заголовок + 15 служебных + 8 Modbus + 2 хвост
const size_t MAX_FRAME = 320;     // с запасом для ответа на 125 регистров

// Запрос "Read Holding Registers" (функция 0x03), упакованный в кадр V5
inline void buildRead(uint8_t* f, uint32_t loggerSn, uint8_t seq, uint8_t slave,
                      uint16_t start, uint16_t count) {
  uint8_t mb[8] = { slave, 0x03, (uint8_t)(start >> 8), (uint8_t)start,
                    (uint8_t)(count >> 8), (uint8_t)count, 0, 0 };
  uint16_t c = crc16(mb, 6);
  mb[6] = (uint8_t)c; mb[7] = (uint8_t)(c >> 8);

  const uint16_t len = 15 + 8;
  f[0] = 0xA5;
  f[1] = (uint8_t)len; f[2] = (uint8_t)(len >> 8);
  f[3] = 0x10; f[4] = 0x45;                       // control code 0x4510 (запрос)
  f[5] = seq;  f[6] = 0x00;                       // номер последовательности
  f[7] = (uint8_t)loggerSn;         f[8]  = (uint8_t)(loggerSn >> 8);
  f[9] = (uint8_t)(loggerSn >> 16); f[10] = (uint8_t)(loggerSn >> 24);
  f[11] = 0x02;                                   // frame type
  for (int i = 12; i <= 25; i++) f[i] = 0;        // sensor type + 3 поля времени
  for (int i = 0; i < 8; i++) f[26 + i] = mb[i];
  uint8_t sum = 0;
  for (int i = 1; i <= 33; i++) sum += f[i];
  f[34] = sum;
  f[35] = 0x15;
}

enum Result {
  OK = 0, HEARTBEAT = 1,
  ERR_FRAME = -1, ERR_CHECKSUM = -2, ERR_SEQ = -3, ERR_SN = -4, ERR_CTRL = -5,
  ERR_MODBUS = -6, ERR_EXCEPTION = -7, ERR_CRC = -8
};

// Полная длина кадра по первым 3 байтам (A5 + длина полезной нагрузки)
inline size_t frameLen(const uint8_t* hdr) { return 13 + (size_t)(hdr[1] | (hdr[2] << 8)); }

// Разбор ответа: регистры в out[0..count-1]
inline Result parseRead(const uint8_t* f, size_t n, uint32_t loggerSn, uint8_t seq,
                        uint8_t slave, uint16_t count, uint16_t* out) {
  if (n < 13 || f[0] != 0xA5 || f[n - 1] != 0x15) return ERR_FRAME;
  if (f[3] == 0x10 && f[4] == 0x47) return HEARTBEAT;      // служебный keep-alive логгера (короткий кадр)
  if (n < 29) return ERR_FRAME;
  uint8_t sum = 0;
  for (size_t i = 1; i < n - 2; i++) sum += f[i];
  if (sum != f[n - 2]) return ERR_CHECKSUM;
  if (f[3] != 0x10 || f[4] != 0x15) return ERR_CTRL;       // 0x1510 = ответ
  if (f[5] != seq) return ERR_SEQ;
  uint32_t sn = (uint32_t)f[7] | ((uint32_t)f[8] << 8) | ((uint32_t)f[9] << 16) | ((uint32_t)f[10] << 24);
  if (sn != loggerSn) return ERR_SN;
  if (f[11] != 0x02) return ERR_FRAME;

  const uint8_t* mb = f + 25;
  size_t mlen = n - 27;
  if (mlen < 5 || mb[0] != slave) return ERR_MODBUS;
  if (mb[1] == 0x83) return ERR_EXCEPTION;
  if (mb[1] != 0x03 || mb[2] != count * 2 || mlen != (size_t)(3 + count * 2 + 2)) return ERR_MODBUS;
  uint16_t c = crc16(mb, mlen - 2);
  if ((uint8_t)c != mb[mlen - 2] || (uint8_t)(c >> 8) != mb[mlen - 1]) return ERR_CRC;
  for (uint16_t i = 0; i < count; i++) out[i] = (uint16_t)((mb[3 + 2 * i] << 8) | mb[4 + 2 * i]);
  return OK;
}

}  // namespace v5
