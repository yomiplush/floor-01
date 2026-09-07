#pragma once
#include <QTcpServer>
#include <QTcpSocket>
#include <QNetworkInterface>
#include <QNetworkProxy>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QTimer>
#include <QUuid>
#include <functional>

// Small, bounded HTTP/1.1 endpoint. One request per connection, no external services.
class LanRemote : public QObject {
    QTcpServer server;
    QByteArray token;
    std::function<QJsonObject()> snapshot;
    std::function<bool(const QJsonObject&)> command;
    int connections=0;
    void reply(QTcpSocket*s,int status,const QByteArray&body,const QByteArray&type="application/json") {
        if(s->property("done").toBool())return;
        s->setProperty("done",true);
        QByteArray reason=status==200?"OK":status==403?"Forbidden":status==404?"Not Found":"Bad Request";
        s->write("HTTP/1.1 "+QByteArray::number(status)+" "+reason+"\r\nContent-Type: "+type+
                 "\r\nContent-Length: "+QByteArray::number(body.size())+
                 "\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
                 "Referrer-Policy: no-referrer\r\nContent-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'\r\n\r\n"+body);
        s->disconnectFromHost();
    }
    void receive(QTcpSocket*s) {
        if(s->property("done").toBool())return;
        QByteArray data=s->property("request").toByteArray()+s->readAll();
        if(data.size()>8192){reply(s,400,"{}");return;}
        s->setProperty("request",data);
        int split=data.indexOf("\r\n\r\n");if(split<0)return;
        auto lines=data.left(split).split('\n');
        auto first=lines.takeFirst().trimmed().split(' ');
        if(first.size()!=3||first[2]!="HTTP/1.1"){reply(s,400,"{}");return;}
        QMap<QByteArray,QByteArray> headers;
        for(auto line:lines){int colon=line.indexOf(':');if(colon<=0){reply(s,400,"{}");return;}
            auto key=line.left(colon).trimmed().toLower();
            if(headers.contains(key)){reply(s,400,"{}");return;}
            headers[key]=line.mid(colon+1).trimmed();}
        const QByteArray hostHeader=headers.value("host");
        const QString host=QString::fromUtf8(hostHeader);
        QHostAddress peer;int headerPort=0;bool ok=false;
        if(host.startsWith('[')){int c=host.indexOf(']');if(c>0&&host.mid(c,2)=="]:"){peer=QHostAddress(host.mid(1,c-1));headerPort=host.mid(c+2).toInt(&ok);}}
        else if(host.count(':')==1){int c=host.lastIndexOf(':');peer=QHostAddress(host.left(c));headerPort=host.mid(c+1).toInt(&ok);}
        if(headers.contains("transfer-encoding")){reply(s,403,"{}");return;}
        if(!ok||headerPort!=server.serverPort()||peer.isNull()||peer!=server.serverAddress()){reply(s,403,"{}");return;}
        if(headers.contains("origin")&&headers["origin"]!="http://"+host.toUtf8()){reply(s,403,"{}");return;}
        bool valid=true;int length=headers.contains("content-length")?headers["content-length"].toInt(&valid):0;
        if(!valid||length<0||length>2048){reply(s,400,"{}");return;}
        if(data.size()<split+4+length)return;
        if(data.size()!=split+4+length){reply(s,400,"{}");return;}
        if(first[0]=="GET"&&first[1]=="/"){
            QFile file(":/web/index.html");if(!file.open(QIODevice::ReadOnly)){reply(s,404,"{}");return;}reply(s,200,file.readAll(),"text/html; charset=utf-8");return;}
        if(headers.value("authorization")!="Bearer "+token){reply(s,403,"{}");return;}
        if(first[0]=="GET"&&first[1]=="/state"){
            reply(s,200,QJsonDocument(snapshot()).toJson(QJsonDocument::Compact));return;}
        if(first[0]=="POST"&&first[1]=="/command"){
            if(headers.value("content-type")!="application/json"){reply(s,400,"{}");return;}
            QJsonParseError error;auto doc=QJsonDocument::fromJson(data.mid(split+4,length),&error);
            bool ok=error.error==QJsonParseError::NoError&&doc.isObject()&&command(doc.object());
            reply(s,ok?200:400,ok?"{\"ok\":true}":"{\"ok\":false}");return;}
        reply(s,404,"{}");
    }
public:
    // RFC 7335 service-continuity addresses can be assigned on tethered links.
    static bool serviceContinuityAddress(const QHostAddress&a) {
        return a.protocol()==QAbstractSocket::IPv4Protocol&&
               (a.toIPv4Address()&0xfffffff8u)==0xc0000000u;
    }
    static bool localAddress(const QHostAddress&a) {
        if(a.protocol()!=QAbstractSocket::IPv4Protocol)return false;
        auto n=a.toIPv4Address();
        return (n>>24)==127||(n>>24)==10||(n>>20)==0xac1||(n>>16)==0xc0a8||(n>>16)==0xa9fe||serviceContinuityAddress(a);
    }
    static bool ipv6Global(const QHostAddress&a) {
        if(a.protocol()!=QAbstractSocket::IPv6Protocol)return false;
        if(a.isLoopback()||!a.scopeId().isEmpty())return false;
        auto ip=a.toIPv6Address();
        if(ip[0]==0xff||ip[0]==0xfe||(ip[0]&0xfe)==0xfc)return false;
        if(ip[10]==0xff&&ip[11]==0xff)return false;
        return true;
    }
    static bool remoteCandidate(const QHostAddress&a) {return localAddress(a)||ipv6Global(a);}
    static QString formatHost(const QHostAddress&a) {
        return a.protocol()==QAbstractSocket::IPv6Protocol?QString("["+a.toString()+"]"):a.toString();
    }
    static QHostAddress defaultAddress() {
        QHostAddress v6;
        for(auto&i:QNetworkInterface::allInterfaces()){
            if(!(i.flags()&QNetworkInterface::IsUp)||i.flags()&QNetworkInterface::IsLoopBack)continue;
            for(auto&a:i.addressEntries()){
                if(localAddress(a.ip()))return a.ip();
                if(v6.isNull()&&ipv6Global(a.ip()))v6=a.ip();
            }
        }
        return v6.isNull()?QHostAddress::LocalHost:v6;
    }
    LanRemote(QObject*parent,std::function<QJsonObject()> read,std::function<bool(const QJsonObject&)> write)
        :QObject(parent),snapshot(std::move(read)),command(std::move(write)) {
        server.setProxy(QNetworkProxy::NoProxy);server.setMaxPendingConnections(8);
        connect(&server,&QTcpServer::newConnection,this,[this]{
            while(auto*s=server.nextPendingConnection()){
                if(connections>=16){s->abort();s->deleteLater();continue;}
                ++connections;s->setReadBufferSize(8193);
                connect(s,&QTcpSocket::disconnected,this,[this,s]{--connections;s->deleteLater();});
                connect(s,&QTcpSocket::readyRead,this,[this,s]{receive(s);});
                QTimer::singleShot(3000,s,[s]{s->abort();});
            }
        });
    }
    bool start(QHostAddress address,quint16 port=18080) {
        if(server.isListening())return true;
        if(!remoteCandidate(address))return false;
        token=QUuid::createUuid().toString(QUuid::Id128).toLatin1();
        return server.listen(address,port);
    }
    bool active() const {return server.isListening();}
    QHostAddress address() const {return server.serverAddress();}
    QString url() const {return QString("http://%1:%2/#%3").arg(formatHost(server.serverAddress())).arg(server.serverPort()).arg(QString::fromLatin1(token));}
    QString error()const{return server.errorString();}
    void stop(){server.close();for(auto*s:server.findChildren<QTcpSocket*>())s->abort();token.clear();}
};
