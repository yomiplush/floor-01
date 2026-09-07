#pragma once
#include <QDialog>
#include <QComboBox>
#include <QClipboard>
#include <QFileInfo>
#include <QVector>
#include <algorithm>
#include <qrencode.h>
#include "lanremote.h"

class CableSyncDialog : public QDialog {
    LanRemote* remote;
    std::function<void()> release;
    QComboBox* networks;
    QLabel *hint, *qr, *link;
    QPushButton* copy;
    void display() {
        qr->clear();link->clear();copy->setEnabled(remote->active());
        if(!remote->active())return;
        link->setText(remote->url());
        auto* code=QRcode_encodeString(remote->url().toUtf8().constData(),0,QR_ECLEVEL_M,QR_MODE_8,1);
        if(!code){qr->setText("QRを生成できません。下のURLを使用してください。");return;}
        const int scale=6, margin=4;
        QImage bitmap((code->width+margin*2)*scale,(code->width+margin*2)*scale,QImage::Format_RGB32);
        bitmap.fill(Qt::white);
        QPainter painter(&bitmap);
        for(int y=0;y<code->width;y++)for(int x=0;x<code->width;x++)
            if(code->data[y*code->width+x]&1)painter.fillRect((x+margin)*scale,(y+margin)*scale,scale,scale,Qt::black);
        painter.end();QRcode_free(code);qr->setPixmap(QPixmap::fromImage(bitmap));
    }
    void scan() {
        const auto previous=remote->active()?remote->address().toString():networks->currentData().toString();
        networks->clear();bool usb=false,v6=false;
        struct Candidate{int prio;QString label;QString ip;};
        QVector<Candidate> candidates;
        for(const auto& iface:QNetworkInterface::allInterfaces()) {
            if(!(iface.flags()&QNetworkInterface::IsUp)||!(iface.flags()&QNetworkInterface::IsRunning)||iface.flags()&QNetworkInterface::IsLoopBack)continue;
            const auto driver=QFileInfo("/sys/class/net/"+iface.name()+"/device/driver").symLinkTarget();
            const bool ipad=driver.endsWith("/ipheth");
            for(const auto& entry:iface.addressEntries()) {
                const auto ip=entry.ip();
                if(ip.isLoopback())continue;
                const bool ipv6=LanRemote::ipv6Global(ip);
                const bool v4=LanRemote::localAddress(ip);
                if(!ipv6&&!v4)continue;
                const auto prefix=entry.prefixLength()>=0?"/"+QString::number(entry.prefixLength()):QString();
                const auto kind=ipad?QString("USB / iPhone・iPad — "):
                    (ipv6&&!v4)?QString("IPv6 テザリング — "):
                    LanRemote::serviceContinuityAddress(ip)?QString("テザリング候補 / "):QString("LAN / ");
                candidates.append({ipad?0:(ipv6&&!v4)?1:2,
                    kind+iface.humanReadableName()+" — PC: "+ip.toString()+prefix,ip.toString()});
                if(ipad)usb=true;
                if(ipv6&&!v4)v6=true;
            }
        }
        std::stable_sort(candidates.begin(),candidates.end(),[](const Candidate&a,const Candidate&b){return a.prio<b.prio;});
        for(auto&c:candidates)networks->addItem(c.label,c.ip);
        int old=networks->findData(previous);if(old>=0)networks->setCurrentIndex(old);
        QString hintText;
        if(usb)hintText="iPhone・iPadのUSBネットワークを検出しました。使用する接続先を選んでください。";
        else if(v6)hintText="モバイルテザリングなどIPv6主体の網ではiPad側がIPv4を取得できない場合があるため、(IPv6)の接続先を選ぶと届きます。";
        else hintText="PCとiPadを同じWi-Fi、または同じiPhoneのインターネット共有に接続して「再検出」。\nUSB共有は、共有元のインターネット共有をON → USB接続 →「信頼」。Linux側のIP割当も必要です。";
        hintText+="\nQRは選択したPCのIPを使用します。iPadのIPやRouterの値は入力不要です。\niOS側に特殊なIP(IPv4サービス継続用など)が表示される場合も、PC側の接続先を選択してください。\n開けない場合は、端末間通信の制限やPCのファイアウォール(IPv6を含む 18080/tcp の許可)を確認してください。";
        hint->setText(hintText);
        if(networks->count()==0)hint->setText(hint->text()+"\n接続可能なIPv4/IPv6がありません。ネットワーク接続後に再検出してください。");
    }
public:
    CableSyncDialog(LanRemote* server,QWidget* parent,std::function<void()> onStop)
        :QDialog(parent),remote(server),release(std::move(onStop)) {
        setWindowTitle("FLOOR//01 — Cable Sync");setMinimumWidth(620);
        auto* layout=new QVBoxLayout(this);
        auto* title=new QLabel("iPad のカメラでQRを読み取り、Safariで開く");layout->addWidget(title);
        hint=new QLabel;hint->setWordWrap(true);layout->addWidget(hint);
        auto* row=new QHBoxLayout;networks=new QComboBox;row->addWidget(networks,1);
        auto* refresh=new QPushButton("再検出");row->addWidget(refresh);layout->addLayout(row);
        auto* start=new QPushButton("選択した接続先のQRを表示");layout->addWidget(start);
        qr=new QLabel;qr->setAlignment(Qt::AlignCenter);layout->addWidget(qr);
        link=new QLabel;link->setTextInteractionFlags(Qt::TextSelectableByMouse);link->setAlignment(Qt::AlignCenter);layout->addWidget(link);
        layout->addWidget(new QLabel("タップ・フェーダー・XY操作をPCと同期します。音はPCから出ます。\nQRとURLは操作する人だけに共有してください。"));
        auto* actions=new QHBoxLayout;copy=new QPushButton("URLをコピー");actions->addWidget(copy);
        auto* stop=new QPushButton("共有を停止");actions->addWidget(stop);
        auto* close=new QPushButton("閉じる（共有を継続）");actions->addWidget(close);layout->addLayout(actions);
        connect(refresh,&QPushButton::clicked,this,[this]{scan();});
        connect(start,&QPushButton::clicked,this,[this]{
            if(networks->currentIndex()<0)return;
            const QHostAddress address(networks->currentData().toString());
            if(remote->active()&&remote->address()!=address){remote->stop();release();}
            if(!remote->start(address))QMessageBox::warning(this,"Cable Sync","共有を開始できません: "+remote->error());
            display();
        });
        connect(copy,&QPushButton::clicked,this,[this]{if(remote->active())QApplication::clipboard()->setText(remote->url());});
        connect(stop,&QPushButton::clicked,this,[this]{remote->stop();release();display();});
        connect(close,&QPushButton::clicked,this,&QDialog::accept);
        scan();display();
        if(!remote->active()&&networks->count()>0)start->click();
    }
};
