
#include <metal_stdlib>
using namespace metal;


#include "ButterflyTypes.h"

uchar4 hsv2rgb(float3 c) {
    float4 K = float4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    float3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    c = c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
    return uchar4(c.r * 255, c.g * 255, c.b * 255, 255);
}

uint8_t channelBlend(uint8_t c1, uint8_t c2, float ratio) {
    return c1 + floor(ratio * (c2 - c1) + 0.5);
}
uchar4 getMultiColorBlend(constant ButterflyData &data, float n, bool circular) {
    if (data.numColors <= 1) {
        return data.colors[0];
    }

    if (n >= 1.0) n = 0.99999f;
    if (n < 0.0) n = 0.0f;
    float realidx = circular ? n * data.numColors : n * (data.numColors - 1);
    int coloridx1 = floor(realidx);
    int coloridx2 = (coloridx1 + 1) % data.numColors;
    float ratio = realidx - float(coloridx1);
    uchar4 ret;
    ret.r = channelBlend(data.colors[coloridx1].r, data.colors[coloridx2].r,  ratio);
    ret.g = channelBlend(data.colors[coloridx1].g, data.colors[coloridx2].g,  ratio);
    ret.b = channelBlend(data.colors[coloridx1].b, data.colors[coloridx2].b,  ratio);
    ret.a = 255;
    return ret;
}
constant float pi2 = 3.14159*2.0;

kernel void ButterflyEffectStyle1(constant ButterflyData &data,
                                  device uchar4* result,
                                  uint index [[thread_position_in_grid]])
{
    int x = index % data.width;
    int y = index / data.width;

    if (x > data.width) return;
    if (y > data.height) return;

    //  This section is to fix the colors on pixels at {0,1} and {1,0}
    if (x == 0 && y == 1) y++;
    if (x == 1 && y == 0) x++;

    float n = abs((x*x - y*y) * sin(data.offset + ((x+y)*pi2 / float(data.height + data.width))));
    float d = x*x + y*y;
    float h = d > 0.001 ? n/d : 0.0;

    if (data.chunks <= 1 || int(h*data.chunks) % data.skip != 0) {
        if (data.colorScheme == 0) {
            result[index] = hsv2rgb(float3(h, 1.0, 1.0));
        } else {
            result[index] = getMultiColorBlend(data, h, false);
        }
    }
}
kernel void ButterflyEffectStyle2(constant ButterflyData &data,
                                  device uchar4* result,
                                  uint index [[thread_position_in_grid]])
{
    int x = index % data.width;
    int y = index / data.width;

    if (x > data.width) return;
    if (y > data.height) return;

    int maxframe = data.height * 2;
    int frame = (data.height * data.curState / 200) % maxframe;

    float f= (frame < maxframe/2) ? frame + 1 : maxframe - frame;
    float x1= (float(x) - data.width/2.0)/f;
    float y1= (float(y) - data.height/2.0)/f;
    float h = sqrt(x1*x1 + y1*y1);

    if (data.chunks <= 1 || int(h*data.chunks) % data.skip != 0) {
        if (data.colorScheme == 0) {
            result[index] = hsv2rgb(float3(h, 1.0, 1.0));
        } else {
            result[index] = getMultiColorBlend(data, h, false);
        }
    }
}
kernel void ButterflyEffectStyle3(constant ButterflyData &data,
                                  device uchar4* result,
                                  uint index [[thread_position_in_grid]])
{
    int x = index % data.width;
    int y = index / data.width;

    if (x > data.width) return;
    if (y > data.height) return;

    int maxframe = data.height * 2;
    int frame = (data.height * data.curState / 200) % maxframe;



    float f = (frame < maxframe/2) ? frame + 1 : maxframe - frame;
    f = f * 0.1 + float(data.height)/60.0;
    float x1 = (x-data.width/2.0)/f;
    float y1 = (y-data.height/2.0)/f;
    float h = sin(x1) * cos(y1);

    if (data.chunks <= 1 || int(h*data.chunks) % data.skip != 0) {
        if (data.colorScheme == 0) {
            result[index] = hsv2rgb(float3(h, 1.0, 1.0));
        } else {
            result[index] = getMultiColorBlend(data, h, false);
        }
    }
}
kernel void ButterflyEffectStyle4(constant ButterflyData &data,
                                  device uchar4* result,
                                  uint index [[thread_position_in_grid]])
{
    int x = index % data.width;
    int y = index / data.width;

    if (x > data.width) return;
    if (y > data.height) return;

    //  This section is to fix the colors on pixels at {0,1} and {1,0}
    if (x == 0 && y == 1) y++;
    if (x == 1 && y == 0) x++;

    float n = ((x*x - y*y) * sin(data.offset + ((x+y)*pi2 / float(data.height+data.width))));
    float d = x*x + y*y;

    float h = d>0.001 ? n/d : 0.0;
    float intpart;
    float fractpart = modf(h , intpart);
    h = fractpart;
    if (h < 0) h = 1.0 + h;

    if (data.chunks <= 1 || int(h*data.chunks) % data.skip != 0) {
        if (data.colorScheme == 0) {
            result[index] = hsv2rgb(float3(h, 1.0, 1.0));
        } else {
            result[index] = getMultiColorBlend(data, h, false);
        }
    }
}
kernel void ButterflyEffectStyle5(constant ButterflyData &data,
                                  device uchar4* result,
                                  uint index [[thread_position_in_grid]])
{
    int x = index % data.width;
    int y = index / data.width;

    if (x > data.width) return;
    if (y > data.height) return;

    //  This section is to fix the colors on pixels at {0,1} and {1,0}
    if (x == 0 && y == 1) y++;
    if (x == 1 && y == 0) x++;

    float n = abs((x*x - y*y) * sin(data.offset + ((x+y)*pi2 / float(data.height*data.width))));
    float d = x*x + y*y;
    float h=d>0.001 ? n/d : 0.0;

    if (data.chunks <= 1 || int(h*data.chunks) % data.skip != 0) {
        if (data.colorScheme == 0) {
            result[index] = hsv2rgb(float3(h, 1.0, 1.0));
        } else {
            result[index] = getMultiColorBlend(data, h, false);
        }
    }
}
