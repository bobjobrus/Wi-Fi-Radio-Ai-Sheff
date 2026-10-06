// Стенд защиты от свиста (firmware/walkie/howl.cpp) в полной цепочке, как в mic_task: петля «микрофон → мост →
// динамик соседней рации → комната» с АРУ. Юнит-тесты ловят грубые поломки, а вклад тонких правил виден только здесь.
//   clang++ -std=c++17 -O2 -I firmware/walkie tools/howl_chain.cpp firmware/walkie/howl.cpp -o /tmp/howl_chain
//   /tmp/howl_chain multi   — 576 передач в комнатах с 6 резонансами (вырезы переходят из передачи в передачу)
//   /tmp/howl_chain fresh   — 294 первые передачи с одним резонансом (добавьте любой 2-й аргумент — список свистящих)
// 30.09.2026 (прошивка 7): multi — без защиты 576, с защитой 114; fresh — 294 → 2. Для сравнения: первая версия
// (b91efa4) 103 / 0, но ложных «тише» в 10 раз больше; переделка 8e5c21c — 148 / 12. Написан проверяющим агентом.
// Full chain as in audio.cpp mic_task: HP ~120 Hz -> HowlGuard (while streaming) -> AGC (gate 20, target 3000,
// 0..36 dB, fast down 30%/frame, up 0.25 dB/frame unless hold_agc) -> limiter 29000 -> ramped gain -> int16 clamp.
// Loop: output of A goes over the bridge (delay D) to B's speaker, room (sum of band-pass modes) back into A's mic.
#include <vector>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include "howl.h"
struct Band { float b0,b2,a1,a2,x1=0,x2=0,y1=0,y2=0; Band(float f,float q){float w=6.2831853f*f/16000,al=sinf(w)/(2*q),a0=1+al;b0=al/a0;b2=-al/a0;a1=-2*cosf(w)/a0;a2=(1-al)/a0;}
  float step(float x){float o=b0*x+b2*x2-a1*y1-a2*y2;x2=x1;x1=x;y2=y1;y1=o;return o;}};
struct Voice { double ph[13]={}; uint32_t n=0,rnd=12345; float sc;
  Voice(float s):sc(s){}
  float next(){int ms=(int)(n/16%500);float f0=140+30*sinf(n/16000.f*3.1f)+8*sinf(n/16000.f*17);float v=0;
    if(ms<300){float env=ms<30?ms/30.f:(ms>260?(300-ms)/40.f:1.f);for(int h=1;h<=12;h++){ph[h]+=6.283185307*f0*h/16000.;float f=f0*h,amp=300.f/h*(1+2*expf(-powf((f-700)/250,2))+1.2f*expf(-powf((f-1200)/300,2)));v+=amp*env*sinf((float)ph[h]);}}
    rnd=rnd*1103515245u+12345u;n++;return sc*(v+((int)(rnd>>16&0xFF)-128)*0.05f);} };
struct M{float f,q,db;};
static float domhz(const float* v, int n){ float best=0,bf=0; for(float f=300;f<=4500;f+=10){float c=2*cosf(6.2831853f*f/16000),s1=0,s2=0; for(int i=0;i<n;i++){float s=v[i]+c*s1-s2;s2=s1;s1=s;} float e=s1*s1+s2*s2-c*s1*s2; if(e>best){best=e;bf=f;}} return bf; }
struct BurstRes { int onset_ms; bool hold_end; float cut_end; bool sustained; float lvl, dom; int dets, grew_dets; int first_det_ms; int howl_start_ms; };
static uint32_t nrs=99; static float noise(){ nrs=nrs*1664525u+1013904223u; return ((int)(nrs>>16&0xFFFF)-32768)/32768.f; }
// talk_f: frames with voice; total_f: burst frames; g_start<0 -> keep AGC state from previous (continuous)
void run(const std::vector<M>& modes, float coup, float Ds, bool guard, int bursts, float g_start, float amb,
         int talk_f, int total_f, std::vector<BurstRes>& out, bool verbose=false){
  HowlGuard g; Voice voice(0.3f);
  std::vector<Band> bands; for(auto&m:modes) bands.emplace_back(m.f,m.q);
  float gain_db=g_start, prev_g=powf(10,gain_db/20); float hp_x=0,hp_y=0; int D=(int)(Ds*16000); int L=32000; uint32_t t_ms=1000;
  float c=powf(10,coup/20);
  for(int b=0;b<bursts;b++){
    std::vector<float> line(L,0); int wpos=0; for(auto&bb:bands){bb.x1=bb.x2=bb.y1=bb.y2=0;}
    gain_db=g_start; prev_g=powf(10,gain_db/20);
    bool was=false;
    double acc=0; int accn=0; std::vector<float> last; BurstRes r{}; r.first_det_ms=-1; r.howl_start_ms=-1; r.onset_ms=-1; Band probe(modes[0].f, modes[0].f/300.f);
    for(int f=0;f<total_f;f++){ float y[320]; bool st=true;
      for(int i=0;i<320;i++){int rp=(wpos+i-D+L)%L; float ac=0; for(size_t k=0;k<bands.size();k++) ac+=powf(10,modes[k].db/20)*bands[k].step(line[rp]);
        float vv=voice.next(); if(f>=talk_f) vv=0; float x=vv+c*ac+amb*noise(); hp_y=0.954f*(hp_y+x-hp_x); hp_x=x; y[i]=hp_y;}
      if(guard && st){ if(!was) g.burst_start(t_ms); bool h0=g.hold_agc(); if(g.process(y,t_ms)){ r.dets++; if(!h0&&g.hold_agc()) r.grew_dets++; if(r.first_det_ms<0) r.first_det_ms=f*20;
          if(verbose) printf("   b%d f=%d det %.0f Hz hold=%d cut=%.0f gain=%.1f\n",b,f,g.notch_hz(g.notches()-1),g.hold_agc(),g.cut_db(),gain_db);} }
      was=st;
      float peak=0,s2=0; for(int i=0;i<320;i++){peak=fmaxf(peak,fabsf(y[i]));s2+=y[i]*y[i];}
      float rms=sqrtf(s2/320);
      if(rms>20){float want=fminf(fmaxf(20*log10f(3000/rms),0),36); if(want<gain_db) gain_db+=(want-gain_db)*0.3f; else if(!(st&&guard&&g.hold_agc())) gain_db+=fminf(0.25f,want-gain_db);}
      float G=powf(10,gain_db/20); if(peak*G>29000) G=29000/peak;
      float o2=0, p2=0;
      for(int i=0;i<320;i++){float gi=prev_g+(G-prev_g)*i/320.f; float v=fminf(fmaxf(y[i]*gi,-32768.f),32767.f); v=(float)(int16_t)v; line[(wpos+i)%L]=v; o2+=v*v; {float pv=probe.step(v); p2+=pv*pv;} if(f>=total_f-50){acc+=v*v; accn++; last.push_back(v);}}
      // howl start (for reference): first frame after voice off? use: output rms > 55 dB with voice off OR tonal share; approximate by out level over voice-only reference later
      if(r.onset_ms<0 && p2>0.5f*o2 && o2>320.f*300*300) r.onset_ms=f*20;
      prev_g=G; wpos=(wpos+320)%L; t_ms+=20;
      if(verbose && f%25==24) printf("     t=%.1f out %.0f dB gain %.1f dom %.0f\n",(f+1)*0.02,10*log10(o2/320+1e-9),gain_db,domhz(y,320));
    }
    r.hold_end=guard&&g.hold_agc(); r.cut_end=guard?g.cut_db():0;
    r.lvl=10*log10(acc/accn+1e-9); r.sustained = r.lvl>50; r.dom=domhz(last.data()+last.size()-3200,3200);
    if(verbose) { printf("  b%d end %.0f dB dom %.0f notches:",b,r.lvl,r.dom); for(int i=0;i<g.notches();i++) printf(" %.0f",g.notch_hz(i)); printf("\n"); }
    out.push_back(r);
    t_ms+=5000;
  }
}
int main(int argc,char**argv){
  const char* mode = argc>1?argv[1]:"multi";
  if(!strcmp(mode,"multi")){           // first-review grid (identical rooms/levels), classified
    int S0=0,S1=0,SN=0,SOB=0,SDET=0,SL=0,BL=0,BH=0,BC=0, bursts=0; std::vector<int> fd;
    for(unsigned seed=1; seed<=12; seed++) for(float coup: {-24.f,-18.f,-12.f}) for(float Ds: {0.18f,0.25f}) {
      srand(seed); std::vector<M> modes; for(int k=0;k<6;k++) modes.push_back({(float)(800+rand()%2600),(float)(5+rand()%6),-(float)(rand()%7)}); modes[0].db=0;
      std::vector<BurstRes> a,b; run(modes,coup,Ds,false,8,20,0,100,250,a); run(modes,coup,Ds,true,8,20,0,100,250,b);
      for(int i=0;i<8;i++){ bursts++; S0+=a[i].sustained; if(b[i].sustained){S1++; if(!b[i].dets) SN++; if(b[i].dom<800||b[i].dom>4000) SOB++; else if(b[i].dets){ SDET++; if(!b[i].hold_end) BL++; else if(b[i].cut_end==0) BH++; else BC++; } }
        if(b[i].first_det_ms>=0) fd.push_back(b[i].first_det_ms); }
    }
    std::sort(fd.begin(),fd.end());
    printf("  breakdown of in-band detected sustained: only-long (no AGC hold) %d, hold but no cut %d, cut>0 %d\n", BL,BH,BC);
    printf("MULTI grid %d bursts: howl w/o guard %d, with guard %d (0 dets %d; howl freq outside 800-4000: %d; detected-but-still-howling in band: %d); first-det median %d ms p90 %d ms (n=%zu)\n",
      bursts,S0,S1,SN,SOB,SDET, fd.empty()?-1:fd[fd.size()/2], fd.empty()?-1:fd[fd.size()*9/10], fd.size());
  } else if(!strcmp(mode,"single")){   // single resonance, loop gain L at the burst-start AGC gain
    float g0=atof(argc>2?argv[2]:"20");
    int S0=0,S1=0,SN=0,bursts=0,b0h=0,b0g=0,nolag=0; std::vector<int> fd, lag; std::vector<int> lagL[5];
    for(float f0: {1200.f,1800.f,2600.f}) for(float Ldb: {3.f,6.f,9.f,12.f,15.f}) for(float Ds: {0.15f,0.2f,0.25f,0.3f}) {
      std::vector<M> modes{{f0,6,0}}; float coup=Ldb-g0;
      std::vector<BurstRes> a,b; run(modes,coup,Ds,false,8,g0,1.0f,100,250,a); run(modes,coup,Ds,true,8,g0,1.0f,100,250,b);
      int s0=0,s1=0; for(int i=0;i<8;i++){ bursts++; S0+=a[i].sustained; s0+=a[i].sustained; if(b[i].sustained){S1++; s1++; if(!b[i].dets) SN++;} if(b[i].first_det_ms>=0) fd.push_back(b[i].first_det_ms);} 
      b0h+=a[0].sustained; b0g+=b[0].sustained; if(a[0].onset_ms>=0 && b[0].first_det_ms>=0) { lag.push_back(b[0].first_det_ms-a[0].onset_ms); lagL[(int)Ldb/3-1].push_back(b[0].first_det_ms);} else nolag++;
      if(s1) printf("  f0 %.0f L %+.0f D %.2f: w/o %d/8, with %d/8 (b0 dets %d first %d ms end %.0f dB dom %.0f)\n",f0,Ldb,Ds,s0,s1,b[0].dets,b[0].first_det_ms,b[0].lvl,b[0].dom);
    }
    std::sort(fd.begin(),fd.end());
    std::sort(lag.begin(),lag.end()); printf("  burst0 detection minus howl onset (no-guard run, >50%% energy at the mode): median %d ms, min %d, max %d, n=%zu, undefined %d\n", lag.empty()?-1:lag[lag.size()/2], lag.empty()?-1:lag[0], lag.empty()?-1:lag.back(), lag.size(), nolag);
    for(int j=0;j<5;j++){ auto&v=lagL[j]; std::sort(v.begin(),v.end()); if(!v.empty()) printf("  L=+%d: burst0 first detection from burst start: median %d ms (min %d max %d)\n",3*(j+1),v[v.size()/2],v[0],v.back()); }
    printf("SINGLE g0=%.0f %d bursts: howl w/o guard %d, with guard %d (0 dets %d); first burst only: %d -> %d of 60; first-det median %d ms p90 %d ms max %d (n=%zu)\n",g0,bursts,S0,S1,SN,b0h,b0g,
      fd.empty()?-1:fd[fd.size()/2], fd.empty()?-1:fd[fd.size()*9/10], fd.empty()?-1:fd.back(), fd.size());
  } else if(!strcmp(mode,"fresh")){   // first transmission only, dense grid
    float g0=20; int n=0,h0=0,h1=0,nod=0; std::vector<int> fd; std::vector<float> lv;
    for(float f0: {1200.f,1500.f,1800.f,2100.f,2400.f,2600.f}) for(float Ldb=3; Ldb<=15.1f; Ldb+=2) for(float Ds=0.15f; Ds<=0.301f; Ds+=0.025f){
      std::vector<M> modes{{f0,6,0}}; std::vector<BurstRes> a,b; run(modes,Ldb-g0,Ds,false,1,g0,1.0f,100,250,a); run(modes,Ldb-g0,Ds,true,1,g0,1.0f,100,250,b);
      n++; lv.push_back(b[0].lvl); h0+=a[0].sustained; if(b[0].sustained){h1++; if(argc>2) printf("  howl: f0 %.0f L %+.0f D %.3f dets %d cut %.0f end %.0f dB dom %.0f\n",f0,Ldb,Ds,b[0].dets,b[0].cut_end,b[0].lvl,b[0].dom);} if(b[0].first_det_ms>=0) fd.push_back(b[0].first_det_ms); else nod++; }
    std::sort(fd.begin(),fd.end());
    std::sort(lv.begin(),lv.end()); int c40=0,c45=0,c55=0; for(float x:lv){c40+=x>40;c45+=x>45;c55+=x>55;} printf("  guarded tail levels: >40 dB %d, >45 %d, >55 %d, top5:",c40,c45,c55); for(size_t i=lv.size()-5;i<lv.size();i++) printf(" %.0f",lv[i]); printf("\n");
    printf("FRESH single-mode first burst: %d cases, howl w/o guard %d, with guard %d; first det median %d ms max %d; never detected %d\n",n,h0,h1,fd[fd.size()/2],fd.back(),nod);
  } else if(!strcmp(mode,"sone")){
    float f0=atof(argv[2]), Ldb=atof(argv[3]), Ds=atof(argv[4]); int nb=argc>5?atoi(argv[5]):1; float g0=20;
    std::vector<M> modes{{f0,6,0}}; std::vector<BurstRes> a,b; if(nb==1) run(modes,Ldb-g0,Ds,false,1,g0,1.0f,100,250,a); run(modes,Ldb-g0,Ds,true,nb,g0,1.0f,100,250,b,true);
  } else if(!strcmp(mode,"one")){
    unsigned seed=atoi(argv[2]); float coup=atof(argv[3]), Ds=atof(argv[4]);
    srand(seed); std::vector<M> modes; for(int k=0;k<6;k++) modes.push_back({(float)(800+rand()%2600),(float)(5+rand()%6),-(float)(rand()%7)}); modes[0].db=0;
    for(auto&m:modes) printf(" %.0f/Q%.0f/%.0f",m.f,m.q,m.db); printf("\n");
    std::vector<BurstRes> b; run(modes,coup,Ds,true,8,20,0,100,250,b,true);
  }
}
