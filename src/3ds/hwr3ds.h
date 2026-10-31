#ifndef __HWR3DS_H__
#define __HWR3DS_H__

extern int hwr3ds_wanted; 


void HWR3DS_InstallDriver(void);
void HWR3DS_BeginFrame(void);
void HWR3DS_EndFrame(int waitvbl);
void HWR3DS_Close(void);
#endif
