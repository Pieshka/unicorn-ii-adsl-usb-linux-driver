#ifndef _CRC_H_
#define _CRC_H_ 

#define CRC32_INITIAL 0xffffffff

#ifdef __cplusplus
extern "C" {
#endif

#ifdef crc32
#undef crc32
#endif

unsigned long crc32(unsigned long initial, unsigned char *mem, int len);
unsigned short crc10(unsigned short crc, unsigned char *mem, int len);
unsigned char hecCompute(unsigned char *p);
int hecCheck(unsigned char *p);

#ifdef __cplusplus
}
#endif

#endif
