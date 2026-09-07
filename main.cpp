#include <QApplication>
#include <QDir>
#include <QMainWindow>
#include <QPainter>
#include <QMouseEvent>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QFileDialog>
#include <QFrame>
#include <QMessageBox>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QSaveFile>
#include <QShortcut>
#include <SDL.h>
#include <array>
#include <vector>
#include <complex>
#include <cmath>
#include <algorithm>
#include <mutex>
#include <iostream>
#include <QSignalBlocker>
#include <QElapsedTimer>
#include "lanremote.h"
#include "cablesync.h"

constexpr int SR=48000, TRACKS=8, STEPS=16, SCENES=8;
constexpr int SPECN=2048, VIZBANDS=60;
constexpr double PI=3.141592653589793;
const char* names[]={"KICK","CLAP","CLOSED HAT","OPEN HAT","PERC","SEQ BASS","STAB","RIDE"};
const char* colors[]={"#ff765c","#ffab66","#ebcc7b","#badb81","#7bd6b5","#b9a0ff","#79baff","#ed9dce"};
using Pattern=std::array<std::array<int,16>,8>;
struct State {
    std::array<Pattern,SCENES> scenes{};
    std::array<int,SCENES> key{}; // per-scene semitone transpose for SEQ BASS / STAB
    std::array<float,8> levels{.95f,.60f,.45f,.38f,.42f,.90f,.50f,.30f};
    std::array<bool,8> mute{};
    std::array<bool,8> sample{true,true,true,true,true,true,false,true}; // true = use bundled open-source sample for the track
    int bpm=128, scene=0, pending=-1, step=-1;
    float swing=.054f, cutoff=1, echo=.12f, drive=.10f, volume=.65f;
    bool playing=false, fill=false, kill=false;
    State(){
        // Each string is one bar of 16 sixteenth-steps; 1 = normal hit, 2 = accent.
        // Scene order: MAIN FLOOR, DUBSTEP, UK GARAGE, DRUM & BASS,
        // TRAP, ELECTRO, BIG ROOM, BREAKS.
        const char* patterns[SCENES][8]={
            {"2000100020001000","0000200000002000","0010001000100010","0000000000000020",
             "0001000000010000","2010001020100020","0000000020000000","0000000000000000"},
            {"2000000010001000","0000100020000000","0010001000100010","0000002000000000",
             "0001000000010001","2010002000102010","0000001000002000","1000000000000000"},
            {"2000001000100010","0000100000002000","0010001000100010","0000100000002000",
             "0001000100010000","2000100010002000","0010001000100000","0000000000000000"},
            {"2000100020001000","0000200000002000","0010101000101010","0000000000000000",
             "0101010101010101","2000100010001000","2000000010000000","1010101010101010"},
            {"2000001010000020","0000000020000000","0010001000100010","0000000000000000",
             "0001000100010001","2000002010102020","0000001000100000","0000000000000000"},
            {"2000100020001000","0000100000002000","0010001000100010","0000100000000000",
             "0101010101010101","2001002010010020","2000200020002000","0000000000000000"},
            {"2000100020001000","0000200000002000","0010001000100010","0000000000000010",
             "0000000000000000","2000000020100020","2000000000000000","1000000000000000"},
            {"2000000110001000","0000200000001000","0010010000100100","0000000100000000",
             "0101011001010101","2001001020010010","2000000010000000","0000100010001000"}
        };
        key={0,-2,0,0,-2,0,0,3};
        for(int a=0;a<SCENES;a++)for(int t=0;t<8;t++)for(int i=0;i<16;i++)
            scenes[a][t][i]=patterns[a][t][i]-'0';
    }
};
struct Voice { int age=SR*10,note=0; double phase=0; float velocity=0, low=0; bool smp=false; double step=0; };
struct Pcm { std::vector<float> mono; int rate=0; bool ok=false; };
static Pcm decodeWav(const QByteArray& d){
    Pcm p;
    if(d.size()<44||d.left(4)!="RIFF")return p;
    const auto* b=reinterpret_cast<const uchar*>(d.constData());
    int pos=12, fmtOff=-1, dataOff=-1, dataLen=0, nch=0, bits=0, rate=0;
    while(pos+8<=d.size()){
        quint32 sz=quint32(b[pos+4])|quint32(b[pos+5])<<8|quint32(b[pos+6])<<16|quint32(b[pos+7])<<24;
        if(qstrncmp(d.constData()+pos,"fmt ",4)==0){fmtOff=pos+8;nch=b[fmtOff+2]|b[fmtOff+3]<<8;rate=b[fmtOff+4]|b[fmtOff+5]<<8|b[fmtOff+6]<<16|b[fmtOff+7]<<24;bits=b[fmtOff+14]|b[fmtOff+15]<<8;}
        else if(qstrncmp(d.constData()+pos,"data",4)==0){dataOff=pos+8;dataLen=int(sz);}
        pos+=8+int(sz)+int(sz&1);
    }
    if(fmtOff<0||dataOff<0||(nch!=1&&nch!=2)||bits!=16||rate<=0||rate>192000)return p;
    int bps=nch*2, frames=dataLen/bps;
    if(frames<=0)return p;
    p.rate=rate;p.mono.reserve(size_t(frames));
    for(int i=0;i<frames;i++){
        const uchar* s=b+dataOff+i*bps;
        int v0=s[0]|s[1]<<8;if(v0&0x8000)v0-=0x10000;
        if(nch==2){int v1=s[2]|s[3]<<8;if(v1&0x8000)v1-=0x10000;v0=(v0+v1)>>1;}
        p.mono.push_back(float(v0)*(1.f/32768.f));
    }
    p.ok=true;return p;
}
struct Engine {
    State state; std::mutex mutex; std::array<Voice,8> voices;
    std::array<std::vector<float>,8> samp; std::array<int,8> sampRate{}; std::array<float,8> sampRef{};
    void loadSample(int t,const QString& path,float ref=0,float trimSec=0){
        QFile f(path);if(!f.open(QIODevice::ReadOnly))return;auto p=decodeWav(f.readAll());
        if(!p.ok||p.mono.empty())return;
        samp[t]=std::move(p.mono);sampRate[t]=p.rate;sampRef[t]=ref;
        if(trimSec>0)samp[t].resize(std::min(samp[t].size(),size_t(std::llround(p.rate*trimSec))));
    }
    std::vector<float> dl=std::vector<float>(SR*2),dr=std::vector<float>(SR*2);
    int di=0; double countdown=0; float lpL=0,lpR=0,peak=0; unsigned rng=1234567;
    std::array<float,256> scope{}; int scopeIndex=0, scopeDivider=0; uint64_t samples=0, triggers=0;
    std::array<float,SPECN> ring{}; int ringPos=0;
    float noise(){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return float(rng)/2147483648.f-1;}
    void reset(){voices={};for(auto&v:voices)v.age=SR*10;std::fill(dl.begin(),dl.end(),0);std::fill(dr.begin(),dr.end(),0);std::fill(ring.begin(),ring.end(),0);ringPos=0;countdown=0;di=0;lpL=lpR=peak=0;state.step=-1;}
    void tick(){
        state.step=(state.step+1)%16;
        if(state.step==0&&state.pending>=0){state.scene=state.pending;state.pending=-1;}
        static const int notes[]={0,0,12,0,7,0,10,0,0,7,0,3,0,10,7,12};
        for(int t=0;t<8;t++){
            int v=state.scenes[state.scene][t][state.step];
            if(state.fill&&(t==1||t==4)&&state.step>=12)v=state.step%2?1:2;
            if(v&&!state.mute[t]){
                int note=t==6?state.key[state.scene]:t==5?notes[state.step]+state.key[state.scene]:notes[state.step];
                voices[t]={0,note,0,v==2?1.f:.64f,0};
                if(state.sample[t]&&!samp[t].empty()){
                    voices[t].smp=true;
                    voices[t].step=double(sampRate[t])/SR;
                    if(t==5)voices[t].step*=std::pow(2.,(note-sampRef[t])/12.);
                }
                if(t==2)voices[3].age=SR*10;
                triggers++;
            }
        }
        countdown+=SR*60.0/state.bpm/4*(state.step%2?1-state.swing:1+state.swing);
    }
    void render(float* out,int frames){
        for(int i=0;i<frames;i++){
            if(state.playing&&countdown<=0)tick();
            if(state.playing)countdown-=1;
            float l=0,r=0; float duck=1;
            if(voices[0].age<SR&&!state.mute[0]&&!state.kill)duck=1-.28f*std::exp(-float(voices[0].age)/SR*11);
            for(int t=0;t<8;t++){
                auto&v=voices[t]; if(v.age>SR*3)continue;
                double time=double(v.age++)/SR; float x=0;
                if(!v.smp){
                double f=55*std::pow(2.,v.note/12.);
                switch(t){
                    case 0:{ // punchy kick: pitch drop 230->45Hz, clipped body + click
                        v.phase+=(45+185*std::exp(-time*52))/SR;
                        float b=std::exp(-time*9);
                        x=std::tanh(float(std::sin(2*PI*v.phase))*1.5f)*b;
                        x+=noise()*.13f*std::exp(-time*260);
                        break;
                    }
                    case 1:{ // snare/clap hybrid: noise hits + tonal body
                        float hits=std::exp(-time*28)+.85f*std::exp(-std::abs(time-.011f)*180)+.6f*std::exp(-std::abs(time-.024f)*150);
                        float tone=std::sin(2*PI*196*time)*std::exp(-time*60)*.7f;
                        x=std::tanh((noise()*hits*1.5f+tone)*.55f)*.8f;break;
                    }
                    case 2:{float n=noise();v.low+=.24f*(n-v.low);x=(n-v.low)*std::exp(-time*150)*.55f;break;}
                    case 3:{float n=noise();v.low+=.2f*(n-v.low);x=(n-v.low)*std::exp(-time*10)*.42f;break;}
                    case 4:v.phase+=(430+560*std::exp(-time*70))/SR;x=std::tanh((std::sin(2*PI*v.phase)+.4f*std::sin(2*PI*v.phase*1.7f))*1.3f)*std::exp(-time*45)*.7f;break;
                    case 5:{ // big saturated saw + sub
                        v.phase+=f/SR;
                        double saw=std::sin(2*PI*v.phase)+.55*std::sin(2*PI*v.phase*1.004)+.38*std::sin(2*PI*v.phase*2)+.25*std::sin(2*PI*v.phase*3)+.15*std::sin(2*PI*v.phase*4.5);
                        double sub=1.05*std::sin(2*PI*(f*.5)*time);
                        double shp=std::tanh(saw*1.7+sub)*(1-std::exp(-time*1800))*std::exp(-time*2.8);
                        v.low+=.14f*(float(shp)-v.low);
                        x=v.low*.9f*duck;break;
                    }
                    case 6:{ // saw-stack minor-ninth stab
                        double a=1-std::exp(-time*380);
                        for(int semitone:{0,3,7,10,14}){
                            double hz=f*4*std::pow(2.,semitone/12.);
                            x+=std::tanh((std::sin(2*PI*hz*time)+.6*std::sin(2*PI*hz*1.003*time))*1.1f)*.07f;
                        }
                        x+=(float)(std::sin(2*PI*f*4*time)*.05);
                        x*=(float)(a*std::exp(-time*2.6)*duck);break;
                    }
                    case 7:x=(noise()*.22f+std::sin(2*PI*6137*time)*std::sin(2*PI*8711*time)*.15f)*std::exp(-time*6);break;
                }
                } else { // open-source sample playback
                    const std::vector<float>&s=samp[t];
                    if(s.empty()||v.phase>=double(int(s.size())-1)){v.age=SR*10;x=0;}
                    else{
                        size_t i=size_t(v.phase);float fr=float(v.phase-i);
                        x=s[i]+(s[i+1]-s[i])*fr;v.phase+=v.step;
                        if(t==5)x*=duck;
                    }
                }
                if(state.mute[t]||(state.kill&&(t==0||t==5)))x=0;
                x*=v.velocity*state.levels[t]; float pan=t==2?-.3f:t==3?.3f:t==4?-.2f:t==7?.4f:0;
                l+=x*(1-pan)*.72f;r+=x*(1+pan)*.72f;
            }
            float a=1-std::exp(-2*PI*(100+18000*state.cutoff*state.cutoff)/SR);
            lpL+=a*(l-lpL);lpR+=a*(r-lpR);
            int delay=int(SR*60.0/state.bpm*.75);int read=(di-delay+int(dl.size()))%int(dl.size());
            float el=dl[read],er=dr[read];dl[di]=lpL+er*.43f;dr[di]=lpR+el*.43f;di=(di+1)%int(dl.size());
            float gain=1+state.drive*3;
            l=std::tanh((lpL+el*state.echo)*gain)*state.volume;
            r=std::tanh((lpR+er*state.echo)*gain)*state.volume;
            out[i*2]=l;out[i*2+1]=r;peak=std::max(std::max(std::abs(l),std::abs(r)),peak*.9998f);
            if(++scopeDivider==8){scopeDivider=0;scope[scopeIndex++%256]=(l+r)*.5f;}
            ring[ringPos]=(l+r)*.5f;ringPos=(ringPos+1)%SPECN;
            samples++;
        }
    }
    static void callback(void* u,Uint8* stream,int bytes){auto&e=*static_cast<Engine*>(u);std::lock_guard<std::mutex> lock(e.mutex);e.render(reinterpret_cast<float*>(stream),bytes/int(sizeof(float)*2));}
};
QByteArray wav(Engine& e,int bars){
    int frames=int(std::llround(SR*60.0/e.state.bpm*4*bars));
    QByteArray data;data.reserve(44+frames*4);
    auto u16=[&](quint16 x){data.append(char(x));data.append(char(x>>8));};
    auto u32=[&](quint32 x){for(int j=0;j<4;j++)data.append(char(x>>(j*8)));};
    data.append("RIFF");u32(36+frames*4);data.append("WAVEfmt ");u32(16);u16(1);u16(2);u32(SR);u32(SR*4);u16(4);u16(16);data.append("data");u32(frames*4);
    std::array<float,1024> buf{};e.reset();e.state.playing=true;e.state.pending=-1;
    for(int p=0;p<frames;p+=512){int n=std::min(512,frames-p);e.render(buf.data(),n);for(int j=0;j<n*2;j++){float fade=std::min(1.f,float(frames-(p+j/2))/240);u16(quint16(qint16(std::clamp(buf[j]*fade,-1.f,1.f)*32767)));}}
    return data;
}
class Grid:public QWidget{
    Engine&e;
public:
    Grid(Engine&en):e(en){setMinimumSize(760,230);setToolTip("左クリック: ON/OFF · 右クリック: アクセント切替");}
    void paintEvent(QPaintEvent*)override{
        QPainter p(this);p.setRenderHint(QPainter::Antialiasing);std::lock_guard<std::mutex>lock(e.mutex);
        float cw=float(width()-148)/16, rh=float(height()-30)/8;
        p.setFont(QFont("Sans",9,QFont::Bold));
        for(int i=0;i<16;i++){p.setPen(e.state.step==i?QColor("#d9ff72"):QColor("#65717e"));p.drawText(QRectF(144+i*cw,0,cw,24),Qt::AlignCenter,QString::number(i+1));}
        for(int t=0;t<8;t++){
            p.setPen(e.state.mute[t]?QColor("#515963"):QColor(colors[t]));p.drawText(QRectF(8,30+t*rh,134,rh),Qt::AlignVCenter,names[t]);
            for(int i=0;i<16;i++){
                QRectF rect(146+i*cw,33+t*rh,cw-6,rh-8);int v=e.state.scenes[e.state.scene][t][i];
                QColor c=v?QColor(colors[t]):QColor(i/4%2?"#232c37":"#1d2530");if(v==1)c=c.darker(155);if(e.state.mute[t])c=c.darker(220);
                p.setBrush(c);p.setPen(e.state.step==i&&e.state.playing?QPen(QColor("#e9ffae"),2):QPen(Qt::NoPen));p.drawRoundedRect(rect,5,5);
                if(v==2){p.setPen(QColor("#101720"));p.drawLine(rect.topLeft()+QPointF(7,7),rect.topLeft()+QPointF(16,7));}
            }
        }
    }
    void mousePressEvent(QMouseEvent*ev)override{
        int t=int((ev->position().y()-30)/((height()-30)/8.));if(ev->position().y()<30||t<0||t>=8)return;
        std::lock_guard<std::mutex>lock(e.mutex);
        if(ev->position().x()<144){e.state.mute[t]=!e.state.mute[t];}
        else{int i=int((ev->position().x()-144)/((width()-148)/16.));if(i>=0&&i<16){int&v=e.state.scenes[e.state.scene][t][i];v=ev->button()==Qt::RightButton?(v==2?1:2):(v?0:1);}}
        update();
    }
};
class Viz:public QWidget{
    Engine&e; std::array<float,VIZBANDS> lvl{}; std::vector<float> win{};
    static void fft(std::complex<float>*a,int n){
        for(int i=1,j=0;i<n;i++){int bit=n>>1;for(;j&bit;bit>>=1)j^=bit;j^=bit;if(i<j)std::swap(a[i],a[j]);}
        for(int len=2;len<=n;len<<=1){std::complex<float> wn(std::cos(float(-2*PI/len)),std::sin(float(-2*PI/len)));for(int i=0;i<n;i+=len){std::complex<float> w(1,0);for(int j=0;j<len/2;j++){std::complex<float> u=a[i+j],v=a[i+j+len/2]*w;a[i+j]=u+v;a[i+j+len/2]=u-v;w*=wn;}}}
    }
public:
    Viz(Engine&en):e(en){setMinimumHeight(110);setAttribute(Qt::WA_OpaquePaintEvent);setToolTip("SPECTRUM · 40 Hz – 12 kHz");win.resize(SPECN);for(int i=0;i<SPECN;i++)win[i]=.5f-.5f*std::cos(float(2*PI*i/(SPECN-1)));}
    void paintEvent(QPaintEvent*)override{
        QPainter p(this);p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(),QColor("#0c141d"));
        std::vector<float>x(SPECN,0);
        {std::lock_guard<std::mutex>lock(e.mutex);for(int i=0;i<SPECN;i++)x[i]=e.ring[(e.ringPos+i)%SPECN];}
        std::vector<std::complex<float>>f(SPECN);
        for(int i=0;i<SPECN;i++)f[i]=std::complex<float>(x[i]*win[i],0);
        fft(f.data(),SPECN);
        const float fmin=40.f,fmax=12000.f;
        std::array<float,VIZBANDS> tgt{};
        float peak=0;
        for(int b=0;b<VIZBANDS;b++){
            int i0=int(fmin*std::pow(fmax/fmin,b/float(VIZBANDS))/SR*SPECN)+1;
            int i1=int(fmin*std::pow(fmax/fmin,(b+1)/float(VIZBANDS))/SR*SPECN);
            i0=std::max(1,i0);i1=std::min(SPECN/2-1,i1);if(i1<=i0)continue;
            double pw=0;for(int k=i0;k<=i1;k++){float re=f[k].real(),im=f[k].imag();pw+=re*re+im*im;}
            float db=10*std::log10(pw/(i1-i0+1)+1e-10);
            tgt[b]=std::pow(std::clamp((db+66)/58.f,0.f,1.f),.72f);
            peak=std::max(peak,tgt[b]);
        }
        float gain=peak>.03f?std::min(1.35f,1.f/peak):0.f; // gentle auto-level: quiet decays, loud eases off the ceiling
        for(int b=0;b<VIZBANDS;b++){
            float t=std::min(1.f,tgt[b]*gain);
            float d=t-lvl[b];
            lvl[b]+=d*(d>0?.32f:.11f); // smooth, calm rise + slow fall (CAVA-like)
        }
        const float w=float(width()),bw=w/VIZBANDS,barW=bw*.44f,hgt=float(height())-12.f;
        p.setPen(Qt::NoPen);
        const QColor base(64,158,216);
        for(int b=0;b<VIZBANDS;b++){
            float v=lvl[b];
            if(v<.01f)continue;
            QColor c=base;c.setAlphaF(.18f+.46f*v);
            p.setBrush(c);
            float bh=hgt*.9f*v;
            float x=b*bw+(bw-barW)*.5f;
            p.drawRect(QRectF(x,hgt-bh+6,barW,bh));
        }
        p.setPen(QColor(23,34,46));p.drawLine(0,hgt+5,w,hgt+5);
        p.setPen(QColor(15,24,33));for(int k=1;k<5;k++){float yy=6+(hgt-6)*k/5.f;p.drawLine(0,yy,w,yy);}
        p.setPen(QColor("#151d26"));p.drawRect(rect().adjusted(0,0,-1,-1));
    }
};
class Pad:public QWidget{
    Engine&e;
public:Pad(Engine&en):e(en){setMinimumSize(245,155);setToolTip("横: フィルターを開く · 縦: ディレイ量");}
    void paintEvent(QPaintEvent*)override{QPainter p(this);p.setRenderHint(QPainter::Antialiasing);p.fillRect(rect(),QColor("#101720"));p.setPen(QColor("#26333f"));for(int i=1;i<8;i++){p.drawLine(i*width()/8,0,i*width()/8,height());p.drawLine(0,i*height()/8,width(),i*height()/8);}std::lock_guard<std::mutex>lock(e.mutex);QPointF pt(e.state.cutoff*width(),(1-e.state.echo)*height());p.setPen(QColor("#d9ff72"));p.drawLine(QPointF(pt.x(),0),QPointF(pt.x(),height()));p.drawLine(QPointF(0,pt.y()),QPointF(width(),pt.y()));p.setBrush(QColor("#d9ff72"));p.drawEllipse(pt,6,6);p.drawText(12,22,"ECHO ↑");p.drawText(12,height()-12,"FILTER →");}
    void set(QMouseEvent*v){std::lock_guard<std::mutex>lock(e.mutex);e.state.cutoff=std::clamp(float(v->position().x()/width()),0.f,1.f);e.state.echo=std::clamp(float(1-v->position().y()/height()),0.f,.85f);update();}
    void mousePressEvent(QMouseEvent*v)override{set(v);}void mouseMoveEvent(QMouseEvent*v)override{set(v);}
};
class Window:public QMainWindow{
    LanRemote*lan=nullptr;QElapsedTimer fillLease;QString fillClient;QPushButton*killButton=nullptr;QMap<QSlider*,QLabel*> sliderLabels;
    QJsonObject remoteState(){
        std::lock_guard<std::mutex>lock(e.mutex);const auto&s=e.state;
        QJsonArray pattern,lv,mt,sm;for(auto&row:s.scenes[s.scene]){QJsonArray r;for(int v:row)r.append(v);pattern.append(r);}for(float v:s.levels)lv.append(v);for(bool v:s.mute)mt.append(v);for(bool v:s.sample)sm.append(v);
        return {{"pattern",pattern},{"levels",lv},{"mute",mt},{"sample",sm},{"scene",s.scene},{"pending",s.pending},{"step",s.step},{"playing",s.playing},{"audio",bool(audio)},{"bpm",s.bpm},{"swing",s.swing},{"drive",s.drive},{"volume",s.volume},{"cutoff",s.cutoff},{"echo",s.echo},{"fill",s.fill},{"kill",s.kill}};
    }
    bool remoteCommand(const QJsonObject&o){
        const QString op=o["op"].toString();const auto value=o["value"];
        auto integer=[&](const char*k,int lo,int hi){auto v=o[k];return v.isDouble()&&std::isfinite(v.toDouble())&&v.toDouble()==v.toInt(-999)&&v.toInt()>=lo&&v.toInt()<=hi;};
        if(op=="bpm"&&!integer("value",70,180))return false;
        if(op=="scene"&&!integer("value",0,SCENES-1))return false;
        if((op=="level"||op=="mute"||op=="sample"||op=="step")&&!integer("track",0,7))return false;
        if(op=="step"&&(!integer("scene",0,SCENES-1)||!integer("step",0,15)||!integer("value",0,2)))return false;
        if((op=="level"||op=="swing"||op=="drive"||op=="volume")&&!integer("value",0,100))return false;
        if((op=="playing"||op=="kill"||op=="mute"||op=="sample"||op=="fill")&&!value.isBool())return false;
        if(op=="xy"){for(auto k:{"x","y"})if(!o[k].isDouble()||!std::isfinite(o[k].toDouble())||o[k].toDouble()<0||o[k].toDouble()>(QString(k)=="y"?.85:1))return false;}
        if(op=="playing"&&value.toBool()&&!audio)return false;
        {
            std::lock_guard<std::mutex>lock(e.mutex);auto&s=e.state;
            if(op=="playing"){if(s.playing!=value.toBool()){s.playing=value.toBool();e.reset();}if(!s.playing){s.fill=false;fillLease.invalidate();fillClient.clear();}}
            else if(op=="scene"){if(s.playing)s.pending=value.toInt();else{s.scene=value.toInt();s.pending=-1;}}
            else if(op=="step")s.scenes[o["scene"].toInt()][o["track"].toInt()][o["step"].toInt()]=value.toInt();
            else if(op=="mute")s.mute[o["track"].toInt()]=value.toBool();
            else if(op=="sample")s.sample[o["track"].toInt()]=value.toBool();
            else if(op=="level")s.levels[o["track"].toInt()]=value.toInt()/100.f;
            else if(op=="bpm")s.bpm=value.toInt();
            else if(op=="swing")s.swing=value.toInt()*.0045f;
            else if(op=="drive")s.drive=value.toInt()/100.f;
            else if(op=="volume")s.volume=value.toInt()/100.f;
            else if(op=="kill")s.kill=value.toBool();
            else if(op=="xy"){s.cutoff=o["x"].toDouble();s.echo=o["y"].toDouble();}
            else if(op=="reset"){s.cutoff=1;s.echo=.12;}
            else if(op=="fill"){
                auto client=o["client"].toString();if(client.isEmpty()||client.size()>128)return false;
                if(fillLease.isValid()&&fillClient!=client)return false;
                s.fill=value.toBool();if(s.fill){fillClient=client;fillLease.start();}else{fillClient.clear();fillLease.invalidate();}
            }
            else if(op=="resetAll"){ // restore factory presets / mixer / FX, keep transport running
                bool was=s.playing;
                s=State{};s.playing=was;
                e.reset();
                if(!s.playing){s.fill=false;fillLease.invalidate();fillClient.clear();}
            }
            else return false;
        }
        // Keep native widgets and browser controls on the same state, without re-emitting writes.
        State copy;{std::lock_guard<std::mutex>lock(e.mutex);copy=e.state;}
        {QSignalBlocker block(bpm);bpm->setValue(copy.bpm);}
        auto syncSlider=[this](QSlider* widget,int value){QSignalBlocker block(widget);widget->setValue(value);if(sliderLabels.contains(widget))sliderLabels[widget]->setText(widget->property("title").toString()+"  "+QString::number(value));};
        syncSlider(swing,int(std::lround(copy.swing/.0045f)));syncSlider(drive,int(std::lround(copy.drive*100)));syncSlider(volume,int(std::lround(copy.volume*100)));
        {QSignalBlocker block(killButton);killButton->setChecked(copy.kill);}
        for(int t=0;t<8;t++){QSignalBlocker block(levels[t]);levels[t]->setValue(int(std::lround(copy.levels[t]*100)));if(smpButtons[t]){QSignalBlocker b2(smpButtons[t]);smpButtons[t]->setChecked(copy.sample[t]);}}
        grid->update();pad->update();return true;
    }
    Engine e;SDL_AudioDeviceID audio=0;Grid*grid;Viz*scope;Pad*pad;QLabel*status;QPushButton*play;std::array<QPushButton*,SCENES> scenes;std::array<QSlider*,8> levels;std::array<QPushButton*,8> smpButtons{};QSpinBox*bpm;QSlider*swing,*drive,*volume;
    template<class F>void mutate(F f){std::lock_guard<std::mutex>lock(e.mutex);f(e.state);}
    QPushButton* button(QString text,QHBoxLayout*l,std::function<void()> action){auto*b=new QPushButton(text);l->addWidget(b);connect(b,&QPushButton::clicked,this,action);return b;}
    QSlider* slider(QString title,int value,QVBoxLayout*l,std::function<void(int)> action){auto*label=new QLabel(title+"  "+QString::number(value));l->addWidget(label);auto*s=new QSlider(Qt::Horizontal);s->setRange(0,100);s->setValue(value);s->setProperty("title",title);sliderLabels[s]=label;l->addWidget(s);connect(s,&QSlider::valueChanged,this,[=](int v){label->setText(title+"  "+QString::number(v));action(v);});return s;}
    QLabel* zone(const QString&t){auto*lb=new QLabel(t);lb->setStyleSheet("color:#8fa0b2;font-size:11px;font-weight:700;letter-spacing:1px");return lb;}
    QFrame* hline(){auto*f=new QFrame;f->setObjectName("hline");f->setFixedHeight(1);f->setStyleSheet("QFrame#hline{background:#26333f;border:none}");return f;}
    void addZone(QVBoxLayout*l,const QString&t){l->addWidget(zone(t));}
    void transport(){std::lock_guard<std::mutex>lock(e.mutex);if(e.state.playing){e.state.playing=false;e.reset();}else{e.reset();e.state.playing=true;}}
    void choose(int n){mutate([=](State&s){if(s.playing)s.pending=n;else{s.scene=n;s.pending=-1;}});}
    void save(){QString path=QFileDialog::getSaveFileName(this,"セッション保存",QDir::homePath()+"/Music/Floor01.json","Floor session (*.json)");if(path.isEmpty())return;QJsonObject o;{std::lock_guard<std::mutex>lock(e.mutex);auto&s=e.state;o["version"]=1;o["bpm"]=s.bpm;o["scene"]=s.scene;o["swing"]=s.swing;o["drive"]=s.drive;o["volume"]=s.volume;o["cutoff"]=s.cutoff;o["echo"]=s.echo;QJsonArray all,lv,mt,sm;for(auto&p:s.scenes){QJsonArray tracks;for(auto&t:p){QJsonArray row;for(int v:t)row.append(v);tracks.append(row);}all.append(tracks);}for(float v:s.levels)lv.append(v);for(bool v:s.mute)mt.append(v);for(bool v:s.sample)sm.append(v);o["patterns"]=all;o["levels"]=lv;o["mute"]=mt;o["sample"]=sm;}QSaveFile f(path);if(!f.open(QIODevice::WriteOnly)||f.write(QJsonDocument(o).toJson())<0||!f.commit())QMessageBox::warning(this,"保存失敗",f.errorString());}
    void load(){QString path=QFileDialog::getOpenFileName(this,"セッションを開く",QDir::homePath()+"/Music","Floor session (*.json)");if(path.isEmpty())return;QFile f(path);if(!f.open(QIODevice::ReadOnly)){QMessageBox::warning(this,"読み込み失敗",f.errorString());return;}QJsonParseError err;auto doc=QJsonDocument::fromJson(f.readAll(),&err);auto o=doc.object();        auto all=o["patterns"].toArray();bool valid=err.error==QJsonParseError::NoError&&o["version"].toInt()==1&&(all.size()==4||all.size()==SCENES)&&o["levels"].toArray().size()==8&&o["mute"].toArray().size()==8;State s;for(int a=0;a<all.size()&&a<SCENES;a++){auto ts=all[a].toArray();valid&=ts.size()==8;for(int t=0;t<ts.size()&&t<8;t++){auto row=ts[t].toArray();valid&=row.size()==16;for(int i=0;i<row.size()&&i<16;i++){int v=row[i].toInt(-1);valid&=v>=0&&v<=2;s.scenes[a][t][i]=std::clamp(v,0,2);}}}if(!valid){QMessageBox::warning(this,"読み込み失敗","有効なFLOOR//01セッションではありません。");return;}
        s.bpm=std::clamp(o["bpm"].toInt(128),70,180);s.scene=std::clamp(o["scene"].toInt(),0,SCENES-1);
        auto val=[&](const char*k,double d,double max){return float(std::clamp(o[k].toDouble(d),0.,max));};s.swing=val("swing",0,.45);s.drive=val("drive",.2,1);s.volume=val("volume",.65,1);s.cutoff=val("cutoff",1,1);s.echo=val("echo",.18,.85);for(int t=0;t<8;t++){s.levels[t]=float(std::clamp(o["levels"].toArray().at(t).toDouble(.5),0.,1.));s.mute[t]=o["mute"].toArray().at(t).toBool();}auto smpArr=o["sample"].toArray();if(smpArr.size()==8)for(int t=0;t<8;t++)s.sample[t]=smpArr.at(t).toBool();{std::lock_guard<std::mutex>lock(e.mutex);e.state=s;e.reset();}bpm->setValue(s.bpm);swing->setValue(int(s.swing/.45*100));drive->setValue(int(s.drive*100));volume->setValue(int(s.volume*100));for(int t=0;t<8;t++)levels[t]->setValue(int(s.levels[t]*100));for(int t=0;t<8;t++)smpButtons[t]->setChecked(s.sample[t]);pad->update();}
    void exportWav(){QString path=QFileDialog::getSaveFileName(this,"現在のシーンを8小節書き出し",QDir::homePath()+"/Music/Floor01.wav","WAV (*.wav)");if(path.isEmpty())return;Engine offline;{std::lock_guard<std::mutex>lock(e.mutex);offline.state=e.state;for(int t=0;t<8;t++){offline.samp[t]=e.samp[t];offline.sampRate[t]=e.sampRate[t];offline.sampRef[t]=e.sampRef[t];}}QApplication::setOverrideCursor(Qt::WaitCursor);auto bytes=wav(offline,8);QSaveFile f(path);bool ok=f.open(QIODevice::WriteOnly)&&f.write(bytes)==bytes.size()&&f.commit();QApplication::restoreOverrideCursor();if(!ok)QMessageBox::warning(this,"書き出し失敗",f.errorString());else QMessageBox::information(this,"書き出し完了","8小節 · 48 kHz / 16-bit / stereo\n"+path);}
public:
    void demoPlay(){std::lock_guard<std::mutex>lock(e.mutex);if(!e.state.playing){e.reset();e.state.playing=true;}}
    bool startLan(bool show=true){
        if(!lan)lan=new LanRemote(this,[this]{return remoteState();},[this](const QJsonObject&o){return remoteCommand(o);});
        auto args=QApplication::arguments();auto address=LanRemote::defaultAddress();int port=18080;
        int a=args.indexOf("--lan-address");if(a>=0)address=QHostAddress(args.value(a+1));
        int p=args.indexOf("--lan-port");if(p>=0)port=args.value(p+1).toInt();
        if(port<1||port>65535||!lan->start(address,quint16(port))){if(show)QMessageBox::warning(this,"LAN接続", "LAN待受を開始できません: "+lan->error());std::cerr<<"LAN start failed\n";return false;}
        std::cout<<"FLOOR_LAN_URL="<<lan->url().toStdString()<<std::endl;
        if(show)showCableSync();
        return true;
    }
    void showCableSync(){
        if(!lan)lan=new LanRemote(this,[this]{return remoteState();},[this](const QJsonObject&o){return remoteCommand(o);});
        auto*d=new CableSyncDialog(lan,this,[this]{
            std::lock_guard<std::mutex>lock(e.mutex);
            if(fillLease.isValid())e.state.fill=false;
            fillLease.invalidate();fillClient.clear();
        });
        d->exec();delete d;
    }
    Window(){
        e.loadSample(0,":/samples/kick.wav");
        e.loadSample(1,":/samples/snare.wav");
        e.loadSample(2,":/samples/hat.wav");
        e.loadSample(3,":/samples/open.wav");
        e.loadSample(4,":/samples/perc.wav");
        e.loadSample(5,":/samples/bass808.wav");
        e.loadSample(7,":/samples/cymbal.wav",0.f,1.5f);
        setWindowTitle("FLOOR//01 — Bass Overdrive");resize(1280,900);
        auto*root=new QWidget;setCentralWidget(root);
        auto*l=new QVBoxLayout(root);l->setContentsMargins(18,10,18,8);l->setSpacing(6);
        auto*head=new QHBoxLayout;auto*brand=new QLabel("FLOOR<span style='color:#d9ff72'>//01</span>");brand->setStyleSheet("font-size:32px;font-weight:900;letter-spacing:2px");head->addWidget(brand);head->addStretch();head->addWidget(new QLabel("BASS OVERDRIVE   /   RHYTHM · BASS · PRESSURE"));button("Cable Sync",head,[this]{showCableSync();});l->addLayout(head);
        auto*transportRow=new QHBoxLayout;play=button("▶  PLAY   [SPACE]",transportRow,[this]{transport();});play->setMinimumWidth(160);play->setFixedHeight(38);bpm=new QSpinBox;bpm->setRange(70,180);bpm->setValue(e.state.bpm);bpm->setSuffix(" BPM");bpm->setFixedHeight(38);transportRow->addWidget(bpm);connect(bpm,&QSpinBox::valueChanged,this,[this](int v){mutate([=](State&s){s.bpm=v;});});transportRow->addStretch();button("INIT",transportRow,[this]{if(QMessageBox::question(this,"INIT","全パターン・ミキサー・FXを初期状態に戻しますか？")==QMessageBox::Yes)remoteCommand(QJsonObject{{"op","resetAll"}});});button("OPEN",transportRow,[this]{load();});button("SAVE",transportRow,[this]{save();});button("EXPORT WAV",transportRow,[this]{exportWav();});l->addLayout(transportRow);
        auto*body=new QHBoxLayout;body->setSpacing(12);
        auto*left=new QVBoxLayout;left->setSpacing(5);
        const char*sn[]={"1 · MAIN FLOOR","2 · DUBSTEP","3 · UK GARAGE","4 · DRUM & BASS","5 · TRAP","6 · ELECTRO","7 · BIG ROOM","8 · BREAKS"};
        auto*sceneRow=new QHBoxLayout;for(int i=0;i<SCENES;i++){scenes[i]=button(sn[i],sceneRow,[this,i]{choose(i);});scenes[i]->setMinimumHeight(32);auto*key=new QShortcut(QKeySequence(QString::number(i+1)),this);connect(key,&QShortcut::activated,this,[this,i]{choose(i);});}left->addLayout(sceneRow);
        addZone(left,"PATTERN GRID   ·   左クリック=ON/OFF · 右クリック=アクセント · 音色名=ミュート");
        grid=new Grid(e);left->addWidget(grid,1);
        auto*leftPad=new QVBoxLayout;addZone(leftPad,"TOUCH FX   ·   横=FILTER · 縦=DELAY");pad=new Pad(e);pad->setMinimumSize(220,96);leftPad->addWidget(pad);left->addLayout(leftPad);
        body->addLayout(left,3);
        auto*right=new QVBoxLayout;right->setSpacing(4);
        const char*ml[]={"KICK","CLAP","CHAT","OHAT","PERC","BASS","STAB","RIDE"};
        addZone(right,"MIXER");
        for(int t=0;t<8;t++){auto*row=new QHBoxLayout;row->setSpacing(5);auto*lab=new QLabel(ml[t]);lab->setFixedWidth(54);lab->setStyleSheet(QString("color:%1;font-size:11px;font-weight:700").arg(colors[t]));levels[t]=new QSlider(Qt::Horizontal);levels[t]->setRange(0,100);levels[t]->setValue(int(e.state.levels[t]*100));levels[t]->setFixedHeight(20);connect(levels[t],&QSlider::valueChanged,this,[this,t](int v){mutate([=](State&s){s.levels[t]=v/100.f;});});smpButtons[t]=new QPushButton;smpButtons[t]->setCheckable(true);connect(smpButtons[t],&QPushButton::toggled,this,[this,t](bool on){mutate([=](State&s){s.sample[t]=on;});smpButtons[t]->setText(on?"SMP":"SYN");smpButtons[t]->setStyleSheet(on?"font-size:9px;padding:0px;background:#d9ff72;color:#111820;border-radius:3px":"font-size:9px;padding:0px;background:#26313e;color:#9fb0c0;border:1px solid #354251;border-radius:3px");});smpButtons[t]->setEnabled(!e.samp[t].empty());smpButtons[t]->setFixedSize(46,20);smpButtons[t]->setToolTip("音源切替: SMP=サンプル / SYN=内蔵シンセ");smpButtons[t]->setChecked(e.state.sample[t]);row->addWidget(lab);row->addWidget(levels[t],1);row->addWidget(smpButtons[t]);right->addLayout(row);}
        addZone(right,"MOTION");
        swing=slider("SWING",12,right,[this](int v){mutate([=](State&s){s.swing=v*.0045f;});});swing->setFixedHeight(20);
        drive=slider("DRIVE",10,right,[this](int v){mutate([=](State&s){s.drive=v/100.f;});});drive->setFixedHeight(20);
        volume=slider("MASTER",65,right,[this](int v){mutate([=](State&s){s.volume=v/100.f;});});volume->setFixedHeight(20);
        right->addStretch(1);
        body->addLayout(right,1);
        l->addLayout(body,1);
        auto*perf=new QHBoxLayout;auto*fill=button("HOLD: FILL",perf,[]{});connect(fill,&QPushButton::pressed,this,[this]{mutate([](State&s){s.fill=true;});});connect(fill,&QPushButton::released,this,[this]{mutate([](State&s){s.fill=false;});});auto*kill=button("LOW CUT",perf,[]{});killButton=kill;kill->setCheckable(true);connect(kill,&QPushButton::toggled,this,[this](bool v){mutate([=](State&s){s.kill=v;});});button("RESET FX",perf,[this]{mutate([](State&s){s.cutoff=1;s.echo=.12;});pad->update();});perf->addStretch();l->addLayout(perf);
        addZone(l,"OUTPUT   ·   SPECTRUM   40 Hz – 12 kHz");
        scope=new Viz(e);l->addWidget(scope);
        status=new QLabel;status->setStyleSheet("color:#8492a0;font-size:11px;padding-top:1px");l->addWidget(status);
        auto*space=new QShortcut(QKeySequence(Qt::Key_Space),this);connect(space,&QShortcut::activated,this,[this]{transport();});
        if(SDL_Init(SDL_INIT_AUDIO)==0){SDL_AudioSpec want{};want.freq=SR;want.format=AUDIO_F32SYS;want.channels=2;want.samples=512;want.callback=Engine::callback;want.userdata=&e;audio=SDL_OpenAudioDevice(nullptr,0,&want,nullptr,0);if(audio)SDL_PauseAudioDevice(audio,0);}
        QString error=audio?QString():QString::fromUtf8(SDL_GetError());if(!audio){play->setEnabled(false);disconnect(space,nullptr,this,nullptr);}
        auto*timer=new QTimer(this);connect(timer,&QTimer::timeout,this,[this,error]{std::lock_guard<std::mutex>lock(e.mutex);if(fillLease.isValid()&&fillLease.elapsed()>1200){e.state.fill=false;fillLease.invalidate();fillClient.clear();}play->setText(e.state.playing?"■  STOP   [SPACE]":"▶  PLAY   [SPACE]");        for(int i=0;i<SCENES;i++)scenes[i]->setStyleSheet(e.state.scene==i?"background:#d9ff72;color:#111820":e.state.pending==i?"background:#5d6540;color:#edffbd":"");status->setText(audio?QString("● AUDIO ONLINE   /   48 kHz · STEREO    |    %1    |    PEAK %2 dBFS%3").arg(e.state.playing?"RUNNING":"READY").arg(20*std::log10(std::max(e.peak,.00001f)),0,'f',1).arg(e.state.pending>=0?"    |    SCENE QUEUED → NEXT BAR":""):"AUDIO UNAVAILABLE: "+error+"  /  WAV export available");grid->update();scope->update();});timer->start(33);
    }
    ~Window(){if(lan){lan->stop();delete lan;}if(audio)SDL_CloseAudioDevice(audio);SDL_Quit();}
};
int main(int argc,char**argv){
    if(argc>1&&QString(argv[1])=="--self-test"){
        Engine e;e.state.playing=true;std::vector<float>b(SR*2*4);e.render(b.data(),SR*4);float peak=0;double sum=0;for(float v:b){if(!std::isfinite(v)||std::abs(v)>1)return 1;peak=std::max(peak,std::abs(v));sum+=v*v;}if(peak<.1||e.triggers<20)return 2;
        e.state.pending=3;std::vector<float>bar(SR*2*3);e.render(bar.data(),SR*3);if(e.state.scene!=3)return 3;
        e.state.playing=false;e.reset();std::array<float,1024>silence{};e.render(silence.data(),512);for(float v:silence)if(v!=0)return 4;
        for(int bpm:{70,128,180})for(float sw:{0.f,.45f}){Engine c;c.state.bpm=bpm;c.state.swing=sw;c.state.playing=true;int frames=int(std::ceil(SR*60.0/bpm*4));std::vector<float>out(frames*2);c.render(out.data(),frames);if(c.state.step!=15)return 5;c.render(out.data(),2);if(c.state.step!=0)return 6;}
        { // bundled CC0 sample playback (kick/snare/hats/perc/808/cymbal; STAB keeps synth)
            Engine s;const char* files[8]={":/samples/kick.wav",":/samples/snare.wav",":/samples/hat.wav",":/samples/open.wav",":/samples/perc.wav",":/samples/bass808.wav","",":/samples/cymbal.wav"};
            for(int t=0;t<8;t++)if(files[t][0])s.loadSample(t,files[t],0.f,t==7?1.5f:0.f);
            s.state.playing=true;s.state.drive=1;s.state.echo=.85;
            std::vector<float>out(SR*8);s.render(out.data(),SR*4);
            double en=0;for(float v:out){if(!std::isfinite(v)||std::abs(v)>1)return 12;en+=v*v;}
            if(en<1||s.triggers<20)return 13;
        }
        for(int scene=0;scene<SCENES;scene++){Engine c;c.state.scene=scene;c.state.playing=true;c.state.drive=1;c.state.echo=.85;std::vector<float>out(SR*8);c.render(out.data(),SR*4);double energy=0;for(float v:out){if(!std::isfinite(v)||std::abs(v)>1)return 9;energy+=v*v;}if(energy<1)return 10;}
        Engine off;auto bytes=wav(off,2);if(bytes.left(4)!="RIFF"||bytes.size()!=44+int(std::llround(SR*60./off.state.bpm*8))*4)return 7;
        if(argc>2){QFile f(argv[2]);if(!f.open(QIODevice::WriteOnly)||f.write(bytes)!=bytes.size())return 8;}
        std::cout<<"PASS: finite bounded audio, peak="<<peak<<", RMS="<<std::sqrt(sum/b.size())<<", scene quantization, stop silence, swing timing, WAV header/length, CC0 sample playback\n";return 0;
    }
    QApplication app(argc,argv);app.setStyle("Fusion");app.setStyleSheet("QWidget{background:#151c25;color:#e4eaf0;font-family:'Noto Sans',sans-serif;font-size:12px} QPushButton{background:#26313e;border:1px solid #354251;border-radius:6px;padding:12px;font-weight:700} QPushButton:hover{border-color:#d9ff72;color:#d9ff72} QPushButton:pressed,QPushButton:checked{background:#d9ff72;color:#111820} QSpinBox{background:#101720;border:1px solid #354251;padding:10px;font-size:18px;font-weight:700} QSlider::groove:horizontal{height:5px;background:#303b47;border-radius:2px} QSlider::handle:horizontal{width:14px;margin:-5px 0;background:#d9ff72;border-radius:6px} QSlider::groove:vertical{width:5px;background:#303b47} QSlider::handle:vertical{height:13px;margin:0 -6px;background:#a2b2c2;border-radius:3px} QToolTip{background:#26313e;color:#e4eaf0}");Window w;w.show();if(app.arguments().contains("--lan")&&!w.startLan(!app.arguments().contains("--lan-quiet")))return 11;if(app.arguments().contains("--smoke-test")){QTimer::singleShot(250,&app,[&]{w.demoPlay();});QTimer::singleShot(1600,&app,[&]{w.grab().save(QDir::current().filePath("screenshot.png"));app.quit();});}return app.exec();
}
