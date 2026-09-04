/* 底层隔离：直接 speex 编码→解码（不经 FRC 容器），逐帧打印 n/ns。 */
#include "speex/speex.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

int esp_get_free_heap_size(void){return 1;}
#define _LOGE(...) fprintf(stderr, __VA_ARGS__); fputc('\n',stderr)
#ifdef SPEEX_SET_QUALITY
#endif

int main(void){
    const int RATE=8000, FRAME=160;
    const int N=160*50; /* 50 帧 */
    int16_t *src=malloc(N*2);
    for(int i=0;i<N;i++){
        double t=(double)i/RATE;
        src[i]=(int16_t)(20000.0*sin(2*M_PI*330.0*t));   /* 恒幅纯音 */
    }

    SpeexBits bits; speex_bits_init(&bits);
    void *enc=speex_encoder_init(&speex_nb_mode);
    int q=3,sr=RATE,vad=0,dtx=0;
    speex_encoder_ctl(enc,SPEEX_SET_QUALITY,&q);
    speex_encoder_ctl(enc,SPEEX_SET_SAMPLING_RATE,&sr);
    speex_encoder_ctl(enc,SPEEX_SET_VAD,&vad);
    speex_encoder_ctl(enc,SPEEX_SET_DTX,&dtx);

    void *dec=speex_decoder_init(&speex_nb_mode);
    speex_decoder_ctl(dec,SPEEX_SET_SAMPLING_RATE,&sr);

    printf("== RAW ROUND TRIP ==\n");
    unsigned char pkt[512];
    int16_t out[FRAME];
    int total_out=0;
    for(int f=0;f<N/FRAME;f++){
        speex_bits_reset(&bits);
        int erc=speex_encode_int(enc,src+f*FRAME,&bits);
        int n=speex_bits_write(&bits,(char*)pkt,512);
        /* 手动解析帧头部：wideband(1) + submode(4) */
        SpeexBits probe; speex_bits_init(&probe);
        speex_bits_read_from(&probe,(const char*)pkt,n);
        int wb = speex_bits_unpack_unsigned(&probe,1);
        int sm = speex_bits_unpack_unsigned(&probe,4);
        speex_bits_destroy(&probe);
        speex_bits_read_from(&bits,(const char*)pkt,n);
        int ns=speex_decode_int(dec,&bits,out);
        /* 成功时 ns==0，实际解出 160 样本 */
        if (f==0){
            int i; printf("pkt0[0:16]="); for(i=0;i<16;i++)printf("%02x ",pkt[i]); printf("\n");
            printf("src[:5]=%d %d %d %d %d  out[:5]=%d %d %d %d %d\n",src[0],src[1],src[2],src[3],src[4],out[0],out[1],out[2],out[3],out[4]);
            printf("out[100:107]=%d %d %d %d %d %d %d\n",out[100],out[101],out[102],out[103],out[104],out[105],out[106]);
        }
        double se=0,sp=0,sx=0,ss=0;
        for(int i=0;i<FRAME;i++){double e=out[i]-src[f*FRAME+i];se+=e*e;double v=src[f*FRAME+i];sp+=v*v;double o=out[i];sx+=o*o;ss+=v*o;}
        double corr = (sp>0&&sx>0)? ss/sqrt(sp*sx) : 0;
        /* 搜索最佳时移（±40 样本），排除帧延迟 */
        int bestlag=0; double bestc=-9;
        {int L=40;
         for(int lag=-L;lag<=L;lag++){
            double sxy=0,pa=0,pb=0;
            for(int i=0;i<FRAME;i++){
                int j=i+lag;
                if(j<0||j>=FRAME)continue;
                sxy+=(double)out[i]*src[f*FRAME+j]; pa+=(double)out[i]*out[i]; pb+=src[f*FRAME+j]*src[f*FRAME+j];
            }
            double c=(pa>0&&pb>0)?sxy/sqrt(pa*pb):0;
            if(c>bestc){bestc=c;bestlag=lag;}
         }}
        double snr = (sp>1e-6 && se>1e-9) ? 10*log10(sp/se):99;
        printf("frame[%d] corr=%.2f bestlag=%d corr@lag=%.2f  (frameSNR=%.1f)\n",f,corr,bestlag,bestc,snr);
        total_out+=FRAME;
    }
    printf("total_decoded=%d expect=%d\n",total_out,N);
    speex_bits_destroy(&bits);
    speex_encoder_destroy(enc);
    speex_decoder_destroy(dec);
    return 0;
}