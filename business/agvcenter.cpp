﻿#include "agvcenter.h"
#include "util/global.h"
#include "util/common.h"

#define _USE_MATH_DEFINES
#include <math.h>
//宏定义一些
//包头
#define AGV_PACK_HEAD    0x55
//包尾
#define AGV_PACK_END   0xAA

//功能码: 手动模式
#define AGV_PACK_SEND_CODE_HAND_MODE    0x33
//功能码：自动模式
#define AGV_PACK_SEND_CODE_AUDTO_MODE   0x35
//功能码:升级
#define AGV_PACK_SEND_CODE_UPDATE_MODE  0x22

//功能码：接收Agv上报状态的
#define AGV_PACK_RECV_CODE_STATUS       0x44

//rfid是立即执行
#define AGV_PACK_SEND_RFID_CODE_IMMEDIATELY       0x00000000
#define AGV_PACK_SEND_RFID_CODE_ETERNITY          0xFFFFFFFF

//指令代码
#define AGV_PACK_SEND_INSTRUC_CODE_STOP      0x00
#define AGV_PACK_SEND_INSTRUC_CODE_FORWARD      0x01
#define AGV_PACK_SEND_INSTRUC_CODE_BACKWARD      0x02
#define AGV_PACK_SEND_INSTRUC_CODE_LEFT      0x03
#define AGV_PACK_SEND_INSTRUC_CODE_RIGHT      0x04
#define AGV_PACK_SEND_INSTRUC_CODE_MP3LEFT      0x05
#define AGV_PACK_SEND_INSTRUC_CODE_MP3RIGHT      0x06
#define AGV_PACK_SEND_INSTRUC_CODE_MP3VOLUME      0x07


enum AGV_HAND_TYPE{
    AGV_HAND_TYPE_STOP = 0,//停止移动
    AGV_HAND_TYPE_FORWARD = 0x1,//前进
    AGV_HAND_TYPE_BACKWARD = 0x2,//后退
    AGV_HAND_TYPE_TURNLEFT = 0x3,//左转
    AGV_HAND_TYPE_TURNRIGHT = 0x4,//右转
};

const char CHAR_NULL = '\0';

AgvCenter::AgvCenter(QObject *parent) : QObject(parent)
{

}

//获取空闲的车辆
QList<Agv *> AgvCenter::getIdleAgvs()
{
    QList<Agv *> result;
    for(QMap<int,Agv *>::iterator itr =g_m_agvs.begin();itr!=g_m_agvs.end();++itr)
    {
        if(itr.value()->myStatus == AGV_STATUS_IDLE){
            result.push_back(itr.value());
        }
    }
    return result;
}
bool AgvCenter::handControlCmd(int agvId,int agvHandType,int speed)
{
    if(!g_m_agvs.contains(agvId))return false;
    //组装一个手控的命令
    QByteArray content;
    content.append(0x33);//手控的功能码
    short baseSpeed = speed & 0xFFFF;
    short forwardSpeed = 0;
    short leftSpeed = 0;
    switch(agvHandType){
    case AGV_HAND_TYPE_STOP:
        break;
    case AGV_HAND_TYPE_FORWARD:
        forwardSpeed = baseSpeed;
        break;
    case AGV_HAND_TYPE_BACKWARD:
        forwardSpeed = -1*baseSpeed;
        break;
    case AGV_HAND_TYPE_TURNLEFT:
        leftSpeed = baseSpeed;
        break;
    case AGV_HAND_TYPE_TURNRIGHT:
        leftSpeed = -1 * baseSpeed;
        break;
    default:
        return false;
    }

    //前后方向 2Byte
    content.append((forwardSpeed>>8) &0xFF);
    content.append((forwardSpeed) &0xFF);

    //左右方向 2Byte
    content.append((leftSpeed>>8) &0xFF);
    content.append((leftSpeed) &0xFF);

    //附件命令 4Byte
    content.append(CHAR_NULL);
    content.append(CHAR_NULL);
    content.append(CHAR_NULL);
    content.append(CHAR_NULL);

    //灯带数据 1Byte
    content.append(CHAR_NULL);

    //控制交接 1Byte
    content.append(CHAR_NULL);

    //设备地址，指令发起者 2Byte
    content.append(CHAR_NULL);
    content.append(CHAR_NULL);

    //备用字节S32*4 = 16Byte
    for(int i=0;i<16;++i){
        content.append(CHAR_NULL);
    }

    QByteArray result = packet(agvId,AGV_PACK_SEND_CODE_HAND_MODE,content);


    //发送命令
    return tcpClient->sendToServer(result.data(),result.length());
}

QByteArray AgvCenter::taskStopCmd(int agvId)
{
    //组装一个agv执行path的命令
    QByteArray content;

    ++g_m_agvs[agvId]->queueNumber;
    g_m_agvs[agvId]->queueNumber &=  0xFF;
    //队列编号 0-255循环使用
    content[0] = g_m_agvs[agvId]->queueNumber;

    //首先需要启动
    //1.立即停止
    content.append(auto_instruct_stop(AGV_PACK_SEND_RFID_CODE_ETERNITY,0));

    //固定长度五组
    while(content.length()+5 < 28){
        content.append(auto_instruct_wait());///////////////////////////////////////////////////////5*5=25Byte
    }

    content.append(CHAR_NULL);
    content.append(CHAR_NULL);/////////////////////////////////////////////设备地址 2Byte

    assert(content.length() == 28);
    //组包//加入包头、功能码、内容、校验和、包尾
    QByteArray result = packet(agvId,AGV_PACK_SEND_CODE_AUDTO_MODE,content);

    return result;
}
QByteArray AgvCenter::taskControlCmd(int agvId)
{
    //组装一个agv执行path的命令
    QByteArray content;

    ++g_m_agvs[agvId]->queueNumber;
    g_m_agvs[agvId]->queueNumber &=  0xFF;
    //队列编号 0-255循环使用
    content[0] = g_m_agvs[agvId]->queueNumber;

    //首先需要启动
    //1.立即启动
    content.append(auto_instruct_forward(AGV_PACK_SEND_RFID_CODE_IMMEDIATELY,g_m_agvs[agvId]->speed));

    //然后对接下来的要执行的数量进行预判
    for(int i=0;i<g_m_agvs[agvId]->currentPath.length() && content.length()+5 < 28;++i){
        AgvLine *line = g_m_lines[g_m_agvs[agvId]->currentPath.at(i)];
        AgvStation *station = g_m_stations[line->endStation];
        //加入一个命令
        content.append(auto_instruct_forward(station->rfid,g_m_agvs[agvId]->speed));
    }

    //固定长度五组
    while(content.length()+5 < 28){
        content.append(auto_instruct_wait());///////////////////////////////////////////////////////5*5=25Byte
    }

    content.append(CHAR_NULL);
    content.append(CHAR_NULL);/////////////////////////////////////////////设备地址 2Byte

    assert(content.length() == 28);
    //组包//加入包头、功能码、内容、校验和、包尾
    QByteArray result = packet(agvId,AGV_PACK_SEND_CODE_AUDTO_MODE,content);

    return result;
}

void AgvCenter::onAgvRead(const char *data,int len)
{
    static QByteArray buffer;
    buffer += QByteArray::fromStdString(std::string(data,len));
    if(buffer.length() >= 32){
        //寻找起点，、、寻找终点
        int start,end;
        while(true){
            start = buffer.indexOf(0x55);
            if(start<0)break;
            end = buffer.indexOf(0xAA,start);
            if(end<0)break;

            //截取这条消息，
            QByteArray oneMsg = buffer.mid(start,end-start);
            int agvid;
            if(start<3){
                buffer = buffer.right(buffer.length()-end-1);
                continue;
            }else{
                int high = buffer.at(start-3);
                int low = buffer.at(start-2);
                agvid = high<<8;
                agvid +=low;
            }
            buffer = buffer.right(buffer.length()-end-1);
            //截取这条消息的agvId
            if(g_m_agvs.contains(agvid)){
                processOneMsg(agvid,oneMsg);
            }
        }
    }

}

void AgvCenter::agvConnectCallBack()
{
    qDebug()<<("agv connect OK!\n");
}

void AgvCenter::agvDisconnectCallBack()
{
    qDebug()<<("agv disconnect!\n");
}

bool AgvCenter::agvStopTask(int agvId)
{
    if(!g_m_agvs.contains(agvId))return false;
    Agv *agv = g_m_agvs[agvId];
    agv->currentPath.clear();
    QByteArray qba =  taskControlCmd(agvId);
    //组包完成，发送
    return tcpClient->sendToServer(qba.data(),qba.length());
}

bool AgvCenter::agvStartTask(int agvId, QList<int> path)
{
    if(!g_m_agvs.contains(agvId))return false;
    Agv *agv = g_m_agvs[agvId];

    //TODO:这里需要启动小车，告诉小车下一站和下几站，还有就是左中右信息(回头再说左中右)
    agv->currentPath = (path);

    //获取path中的下一站
    if(agv->nowStation!=g_m_lines[agv->currentPath.at(0)]->startStation){
        agv->nextStation = g_m_lines[agv->currentPath.at(0)]->startStation;
    }else{
        agv->nextStation = g_m_lines[agv->currentPath.at(0)]->endStation;
    }

    QByteArray qba =  taskControlCmd(agvId);
    //组包完成，发送
    return tcpClient->sendToServer(qba.data(),qba.length());
}

void AgvCenter::processOneMsg(int id, QByteArray oneMsg)
{
    if(!g_m_agvs.contains(id))return;
    Agv *agv = g_m_agvs[id];
    //////////////////////////oneMsg组成部分
    /// 1Byte 包头 0x55
    /// 1Byte 功能码 (理论上只有0x44)
    /// 2Byte 包长 0x44的包长确定是32
    /// 32Byte 包内容
    /// 1Byte 校验和


    /////////////////////////////////具体包内容解析(32Byte)
    /// 4Byte 开机里程
    /// 4Byte 开机弧度 628 = 360
    /// 4Byte RFID(直到遇到下一个更新)
    /// 1Byte 速度 单位是0.1m/s
    /// 1Byte 转向速度 0.1°/s
    /// 1Byte 小车忙率 CPU
    /// 1Byte 小车状态
    /// 1Byte 左电机状态
    /// 1Byte 右电机状态
    /// 2Byte 系统电压 0.01V
    /// 2Byte 系统电流 0.01A
    /// 2Byte 磁条位置
    /// 1Byte 前方障碍
    /// 1Byte 后方障碍
    /// 1Byte 当前命令
    /// 1Byte 队列编号
    /// 2Byte 设备地址
    /// 2Byte 附件状态



    ///////////////////////////////////////////总结一下
    /// 1Byte 包头 0x55
    /// 1Byte 功能码 (理论上只有0x44)
    /// 2Byte 包长 0x44的包长确定是32
    ///
    /// 4Byte 开机里程
    /// 4Byte 开机弧度 628 = 360
    /// 4Byte RFID(直到遇到下一个更新)
    /// 1Byte 速度 单位是0.1m/s
    /// 1Byte 转向速度 0.1°/s
    /// 1Byte 小车忙率 CPU
    /// 1Byte 小车状态
    /// 1Byte 左电机状态
    /// 1Byte 右电机状态
    /// 2Byte 系统电压 0.01V
    /// 2Byte 系统电流 0.01A
    /// 2Byte 磁条位置
    /// 1Byte 前方障碍
    /// 1Byte 后方障碍
    /// 1Byte 当前命令
    /// 1Byte 队列编号
    /// 2Byte 设备地址
    /// 2Byte 附件状态
    ///
    /// 1Byte 校验和

    unsigned char data[256];
    memcpy(data,oneMsg.data(),oneMsg.length());

    int functionCode = data[1];

    if(functionCode == 0x44)
    {
        //里程
        agv->mileage = (data[4]<<24)|(data[5]<<16)|(data[6]<<8)|data[7];
        //角度
        agv->rad = (data[8]<<24)|(data[9]<<16)|(data[10]<<8)|data[11];//这个值恐怕只供参考了//TODO:
        //rfid号
        int station = 0;
        int rfid =  (data[12]<<24)|(data[13]<<16)|(data[14]<<8)|data[15];
        agv->currentRfid = rfid;
        for(QMap<int,AgvStation *>::iterator itr = g_m_stations.begin();itr!=g_m_stations.end();++itr){
            if(itr.value()->rfid == rfid){
                station = itr.value()->id;
                break;
            }
        }
        if(station != 0){
            if(station == agv->nowStation || (agv->nowStation==0 &&station == agv->lastStation )){//上一站未变，只是更新了一下里程计
                updateOdometer(agv,agv->mileage);
            }else{
                updateStationOdometer(agv,station,agv->mileage);
            }
        }

        //速度
        agv->speed = (data[16]);
        //转向速度
        agv->turnSpeed = (data[17]);
        //cpu
        agv->cpu = (data[18]);
        //状态机 status//位置书
        agv->status = (data[19]);
        //左右电机状态
        agv->leftMotorStatus = (data[20]);
        agv->rightMotorStatus = (data[21]);

        //系统电压
        agv->systemVoltage = ((data[22] << 8) |data[23]);
        //系统电流
        agv->systemCurrent = ((data[24] << 8) | data[25]);

        //磁条位置
        agv->positionMagneticStripe = ((data[26]<<8)|data[27]);
        //受障情况
        agv->frontObstruct = (data[28]);
        agv->backObstruct = (data[29]);

        //TODO:当前命令: 0中控控制，1手柄控制 2自动充电
        agv->currentOrder = data[30];

        //TODO:当前队列编号
        agv->currentQueueNumber = data[31];

        //设备地址//TODO
        //判断
        //        QString ss = QString("from agv ip:%1.%2.%3.%4").arg(data[32]).arg(data[33]).arg(data[34]).arg(data[35]);
        //        g_log->log(AGV_LOG_LEVEL_INFO,ss);

        //附件状态 U16

        //检验和
    }
}

//1.只有里程计
void AgvCenter::updateOdometer(Agv *agv, int odometer)
{
    if(odometer == agv->lastStationOdometer)//在原来的位置没动
        return ;

    //正在移动中，不再原来的站点的位置了
    if(agv->nowStation > 0 && agv->nextStation > 0)
    {
        //如果之前在一个站点，现在相当于离开了那个站点
        agv->lastStation = agv->nowStation;
        agv->nowStation = 0;
    }

    if(agv->lastStation <= 0) return ;//上一站未知，那么未知直接就是未知的
    if(agv->nextStation <= 0) return ;//下一站未知，那么我不知道方向。

    //如果两个都知道了，那么我就可以计算当前位置了
    odometer -= agv->lastStationOdometer;

    //例程是否超过了到下一个站点的距离
    if(agv->currentPath.length()<=0)
        return ;
    AgvLine *line =g_m_lines[agv->currentPath.at(0)];
    if(odometer < line->length*line->rate){
        //计算位置
        if(line->line){
            double theta = atan2(g_m_stations[agv->nextStation]->y-g_m_stations[agv->lastStation]->y,g_m_stations[agv->nextStation]->x-g_m_stations[agv->lastStation]->x);
            agv->rotation = (theta*180/M_PI);
            agv->x = (g_m_stations[agv->lastStation]->x+1.0*odometer/line->rate*cos(theta));
            agv->y = (g_m_stations[agv->lastStation]->y+1.0*odometer/line->rate*sin(theta));
        }else{

            //在新的绘图下，计算当前坐标，以及rotation
            double t = 1.0*odometer/line->rate/line->length;
            if(t<0){
                t = 0.0;
            }
            if(t>1){
                t=1.0;
            }
            //计算坐标
            double startX = g_m_stations[line->startStation]->x;
            double startY = g_m_stations[line->startStation]->y;
            double endX = g_m_stations[line->endStation]->x;
            double endY = g_m_stations[line->endStation]->y;
            agv->x = (startX*(1-t)*(1-t)*(1-t)
                      +3*line->p1x*t*(1-t)*(1-t)
                      +3*line->p2x*t*t*(1-t)
                      +endX*t*t*t);

            agv->y = (startY*(1-t)*(1-t)*(1-t)
                      +3*line->p1y*t*(1-t)*(1-t)
                      +3*line->p2y*t*t*(1-t)
                      +endY*t*t*t);

            double X = startX * 3 * (1 - t)*(1 - t) * (-1) +
                    3 * line->p1x * ((1 - t) * (1 - t) + t * 2 * (1 - t) * (-1)) +
                    3 * line->p2x * (2 * t * (1 - t) + t * t * (-1)) +
                    endX * 3 * t *t;

            double Y =  startY * 3 * (1 - t)*(1 - t) * (-1) +
                    3 *line->p1y * ((1 - t) *(1 - t) + t * 2 * (1 - t) * (-1)) +
                    3 * line->p2y * (2 * t * (1 - t) + t * t * (-1)) +
                    endY * 3 * t *t;

            agv->rotation = (atan2(Y, X) * 180 / M_PI);
        }
    }
}

//2.有站点信息和里程计信息
void AgvCenter::updateStationOdometer(Agv *agv, int station, int odometer)
{
    if(!g_m_stations.contains(station))return ;
    //更新当前位置

    //到达了这么个站点
    agv->x = (g_m_stations[station]->x);
    agv->y = (g_m_stations[station]->y);

    //设置当前站点
    agv->nowStation=station;
    agv->lastStationOdometer=odometer;

    //获取path中的下一站
    int nextStationTemp = 0;
    for(int i=0;i<agv->currentPath.length();++i){
        if(g_m_lines[agv->currentPath.at(i)]->endStation == station ){
            if(i+1!=agv->currentPath.length())
                nextStationTemp = g_m_lines[agv->currentPath.at(i+1)]->endStation;
            else
                nextStationTemp = 0;
            break;
        }
    }
    agv->nextStation = nextStationTemp;

    //到站消息上报(更新任务信息、更新线路占用问题)
    emit carArriveStation(agv->id,station);
}


bool AgvCenter::load()//从数据库载入所有的agv
{
    QString querySql = "select id,agv_name from agv_agv";
    QList<QVariant> params;
    QList<QList<QVariant> > result = g_sql->query(querySql,params);
    for(int i=0;i<result.length();++i){
        QList<QVariant> qsl = result.at(i);
        if(qsl.length() == 2){
            Agv *agv = new Agv;
            agv->id=(qsl.at(0).toInt());
            agv->name=(qsl.at(1).toString());
            g_m_agvs.insert(agv->id,agv);
        }
    }
    return true;
}

bool AgvCenter::save()//将agv保存到数据库
{
    //查询所有的，
    QList<int> selectAgvIds;
    QList<QVariant> params;
    QString querySql = "select id from agv_agv";
    QList<QList<QVariant>> queryResult = g_sql->query(querySql,params);
    for(int i=0;i<queryResult.length();++i){
        QList<QVariant> qsl = queryResult.at(i);
        if(qsl.length()==1){
            int id = qsl.at(0).toInt();
            selectAgvIds.push_back(id);
        }
    }

    for(int i=0;i<selectAgvIds.length();++i)
    {
        if(g_m_agvs.contains(selectAgvIds.at(i))){
            //含有,进行更新
            QString updateSql = "update agv_agv set agv_name=? where id=?";
            params.clear();
            params<<g_m_agvs[selectAgvIds.at(i)]->name<<QString("%1").arg(g_m_agvs[selectAgvIds.at(i)]->id);
            if(!g_sql->exeSql(updateSql,params))
                return false;
        }else{
            //不含有，就删除
            QString deleteSql = "delete from agv_agv where id=?";
            params.clear();params.push_back(QString("%1").arg(selectAgvIds.at(i)));
            if(g_sql->exeSql(deleteSql,params)){
                selectAgvIds.removeAt(i);
                --i;
            }else{
                return false;
            }
        }
    }

    //如果在g_m_agvs中有更多的呢，怎么呢，插入
    for(QMap<int,Agv *>::iterator itr=g_m_agvs.begin();itr!=g_m_agvs.end();++itr){
        if(selectAgvIds.contains(itr.key()))continue;
        //插入操作
        QString insertSql = "insert into agv_agv(id,agv_name) values(?,?)";
        params.clear();
        params<<QString("%1").arg(itr.value()->id)<<itr.value()->name;
        if(!g_sql->exeSql(insertSql,params))return false;
    }
    return true;
}



void AgvCenter::init()
{
    load();

    QyhTcp::QyhClientReadCallback _readcallback = std::bind(&AgvCenter::onAgvRead, this, std::placeholders::_1, std::placeholders::_2);
    QyhTcp::QyhClientConnectCallback _connectcallback = std::bind(&AgvCenter::agvConnectCallBack,this);
    QyhTcp::QyhClientDisconnectCallback _disconnectcallback = std::bind(&AgvCenter::agvDisconnectCallBack,this);

    if(tcpClient){
        delete tcpClient;
    }

    tcpClient = QyhTcp::QyhTcpClient::create("127.0.0.1",9999,_readcallback,_connectcallback,_disconnectcallback);
}

QByteArray AgvCenter::auto_instruct_wait(){
    QByteArray qba;
    qba.append(0xFF);
    qba.append(0xFF);
    qba.append(0xFF);
    qba.append(0xFF);
    qba.append(0xFF);
    return qba;
}

QByteArray AgvCenter::auto_instruct_stop(int rfid,int delay)
{
    QByteArray qba;
    qba.append(((rfid>>24) & 0xFF));
    qba.append(((rfid>>16) & 0xFF));
    qba.append(((rfid>>8) & 0xFF));
    qba.append(((rfid) & 0xFF));
    qba.append(((AGV_PACK_SEND_INSTRUC_CODE_STOP<<4)&0xF0)|(delay & 0x0F));
    return qba;
}

QByteArray AgvCenter::auto_instruct_forward(int rfid,int speed)
{
    QByteArray qba;
    qba.append(((rfid>>24) & 0xFF));
    qba.append(((rfid>>16) & 0xFF));
    qba.append(((rfid>>8) & 0xFF));
    qba.append(((rfid) & 0xFF));
    qba.append(((AGV_PACK_SEND_INSTRUC_CODE_FORWARD<<4)&0xF0)|(speed & 0x0F));
    return qba;
}

QByteArray AgvCenter::auto_instruct_backward(int rfid,int speed)
{
    QByteArray qba;
    qba.append(((rfid>>24) & 0xFF));
    qba.append(((rfid>>16) & 0xFF));
    qba.append(((rfid>>8) & 0xFF));
    qba.append(((rfid) & 0xFF));
    qba.append(((AGV_PACK_SEND_INSTRUC_CODE_BACKWARD<<4)&0xF0)|(speed & 0x0F));
    return qba;
}

QByteArray AgvCenter::auto_instruct_turnleft(int rfid,int angle)
{
    QByteArray qba;
    qba.append(((rfid>>24) & 0xFF));
    qba.append(((rfid>>16) & 0xFF));
    qba.append(((rfid>>8) & 0xFF));
    qba.append(((rfid) & 0xFF));
    qba.append(((AGV_PACK_SEND_INSTRUC_CODE_LEFT<<4)&0xF0)|(angle & 0x0F));
    return qba;
}

QByteArray AgvCenter::auto_instruct_turnright(int rfid,int speed)
{
    QByteArray qba;
    qba.append(((rfid>>24) & 0xFF));
    qba.append(((rfid>>16) & 0xFF));
    qba.append(((rfid>>8) & 0xFF));
    qba.append(((rfid) & 0xFF));
    qba.append(((AGV_PACK_SEND_INSTRUC_CODE_RIGHT<<4)&0xF0)|(speed & 0x0F));
    return qba;
}

QByteArray AgvCenter::auto_instruct_mp3_left(int rfid, int mp3Id)
{
    QByteArray qba;
    qba.append(((rfid>>24) & 0xFF));
    qba.append(((rfid>>16) & 0xFF));
    qba.append(((rfid>>8) & 0xFF));
    qba.append(((rfid) & 0xFF));
    qba.append(((AGV_PACK_SEND_INSTRUC_CODE_MP3LEFT<<4)&0xF0)|(mp3Id>>4 & 0x0F));
    return qba;
}

QByteArray AgvCenter::auto_instruct_mp3_right(int rfid,int mp3Id)
{
    QByteArray qba;
    qba.append(((rfid>>24) & 0xFF));
    qba.append(((rfid>>16) & 0xFF));
    qba.append(((rfid>>8) & 0xFF));
    qba.append(((rfid) & 0xFF));
    qba.append(((AGV_PACK_SEND_INSTRUC_CODE_MP3RIGHT<<4)&0xF0)|(mp3Id & 0x0F));
    return qba;
}

QByteArray AgvCenter::auto_instruct_mp3_volume(int rfid,int volume)
{
    QByteArray qba;
    qba.append(((rfid>>24) & 0xFF));
    qba.append(((rfid>>16) & 0xFF));
    qba.append(((rfid>>8) & 0xFF));
    qba.append(((rfid) & 0xFF));
    qba.append(((AGV_PACK_SEND_INSTRUC_CODE_MP3VOLUME<<4)&0xF0)|(volume&0x0F));
    return qba;
}

//将内容封包
//加入包头、(功能码)、包长、(内容)、校验和、包尾
QByteArray AgvCenter::packet(int id, char code_mode, QByteArray content)
{
    //计算校验和
    unsigned char sum = checkSum((unsigned char *)content.data(),content.length());

    //组包//加入包头、功能码、内容、校验和、包尾
    QByteArray result;
    //目标地址、频段
    short _id = id & 0xFFFF;
    char chanel = 0;
    result.append((_id>>8)&0xFF);
    result.append(_id&0xFF);
    result.append(chanel);
    result.append(AGV_PACK_HEAD);//包头
    result.append(code_mode);//功能码

    //包长2Byte
    int contentLength = content.length();
    result.append((contentLength>>8) & 0xFF);
    result.append((contentLength) & 0xFF);

    result.append(content);//内容
    result.append(sum);//校验和
    result.append(AGV_PACK_END);//包尾

    return result;
}
