#include <stdio.h>
#include <queue>
#include <map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <ros/ros.h>
#include <cv_bridge/cv_bridge.h>
#include <opencv2/opencv.hpp>
#include <gnss_comm/gnss_ros.hpp>
#include <gnss_comm/gnss_utility.hpp>
#include <gvins/LocalSensorExternalTrigger.h>
#include <sensor_msgs/NavSatFix.h>

#include "estimator.h"
#include "parameters.h"
#include "utility/visualization.h"

using namespace gnss_comm;

#define MAX_GNSS_CAMERA_DELAY 0.05

std::unique_ptr<Estimator> estimator_ptr;

std::condition_variable con;
double current_time = -1;
queue<sensor_msgs::ImuConstPtr> imu_buf;
queue<sensor_msgs::PointCloudConstPtr> feature_buf;
queue<std::vector<ObsPtr>> gnss_meas_buf;
queue<sensor_msgs::PointCloudConstPtr> relo_buf;
int sum_of_wait = 0;

std::mutex m_buf;
std::mutex m_state;
std::mutex i_buf;
std::mutex m_estimator;

/*** IMU预积分相关参数 ***/
double latest_time;             // 上一帧IMU数据的时间戳（用于IMU预积分）
Eigen::Vector3d tmp_P;          // 平移（临时量）
Eigen::Quaterniond tmp_Q;       // 旋转（临时量）
Eigen::Vector3d tmp_V;          // 速度（临时量）
Eigen::Vector3d tmp_Ba;         // IMU加速度计偏置（临时量）
Eigen::Vector3d tmp_Bg;         // IMU陀螺仪偏置（临时量）
Eigen::Vector3d acc_0;          // 上一帧IMU加速度测量值
Eigen::Vector3d gyr_0;          // 上一帧IMU角速度测量值
bool init_feature = 0;          // 未使用
bool init_imu = 1;              // 是否是第一帧IMU数据
double last_imu_t = -1;         //  上一帧IMU数据的时间戳（用于判断IMU数据时间是否正常，初始值为-1）

std::mutex m_time;              // PPS互斥锁
double next_pulse_time;         // PPS触发时间
bool next_pulse_time_valid;     // 如果进入PPS触发的回调函数gnss_tp_info_callback，那么这个就会设置成true
double time_diff_gnss_local;    // 时间改正数：PPS触发时，VI传感器的本地时间和GPS时间的差值
bool time_diff_valid;           // 如果这个是false，则对于收到的gnss观测数据直接不会存储
double latest_gnss_time;        // 上一帧GNSS数据的时间戳（初始值为-1）
double tmp_last_feature_time;   // 上一帧图像特征数据的时间戳（初始值为-1）
uint64_t feature_msg_counter;   // 图像特征消息计数
int skip_parameter;

/**
 * @brief 基于IMU测量数据进行PVQ状态预测（位置、速度、姿态）
 * 
 * @details 本函数实现惯性导航的机械编排(Mechanical Alignment)，通过积分IMU的角速度和线加速度数据，
 *          更新载体的姿态、速度和位置预测值。通常用于组合导航或视觉惯性里程计(VIO)的预测步骤。
 * 
 * @param[in] imu_msg IMU消息，包含线加速度和角速度测量值（传感器坐标系下）
 */
void predict(const sensor_msgs::ImuConstPtr &imu_msg)
{
    // 1.处理IMU时间戳
    // 获取当前IMU时间戳（转换为秒）
    double t = imu_msg->header.stamp.toSec();

    // 记录第一帧IMU时间戳
    if (init_imu)
    {
        latest_time = t;
        init_imu = 0;
        return;
    }

    // 计算两次IMU测量的时间间隔（单位：秒）
    double dt = t - latest_time;

    // 更新上一帧IMU数据的时间戳
    latest_time = t;

    
    // 2.提取IMU测量值（传感器坐标系下）
    // 线加速度（m/s²）
    double dx = imu_msg->linear_acceleration.x;
    double dy = imu_msg->linear_acceleration.y;
    double dz = imu_msg->linear_acceleration.z;
    Eigen::Vector3d linear_acceleration{dx, dy, dz};

    // 角速度（rad/s）
    double rx = imu_msg->angular_velocity.x;
    double ry = imu_msg->angular_velocity.y;
    double rz = imu_msg->angular_velocity.z;
    Eigen::Vector3d angular_velocity{rx, ry, rz};

    // 3.状态更新
    // 计算校正后的上一帧IMU加速度
    Eigen::Vector3d un_acc_0 = tmp_Q * (acc_0 - tmp_Ba) - estimator_ptr->g;
    // 计算校正后的角速度（中值积分）
    Eigen::Vector3d un_gyr = 0.5 * (gyr_0 + angular_velocity) - tmp_Bg;

    // 3.1 姿态更新：q_{k+1} = q_k ⊗ Δq(ω*dt)
    tmp_Q = tmp_Q * Utility::deltaQ(un_gyr * dt);

    // 计算校正后的当前IMU加速度
    Eigen::Vector3d un_acc_1 = tmp_Q * (linear_acceleration - tmp_Ba) - estimator_ptr->g;

    // 校正后的加速度中值积分
    Eigen::Vector3d un_acc = 0.5 * (un_acc_0 + un_acc_1);

    // 3.2 位置更新：P_{k+1} = P_k + V_k*dt + 0.5*a*dt²（二阶泰勒展开）
    tmp_P = tmp_P + dt * tmp_V + 0.5 * dt * dt * un_acc;
    
    // 3.3 速度更新：V_{k+1} = V_k + a*dt
    tmp_V = tmp_V + dt * un_acc;

    // 4.缓存当前IMU测量值用于下一帧计算
    acc_0 = linear_acceleration;    // 缓存当前加速度
    gyr_0 = angular_velocity;       // 缓存当前角速度
}

void update()
{
    TicToc t_predict;
    latest_time = current_time;
    tmp_P = estimator_ptr->Ps[WINDOW_SIZE];
    tmp_Q = estimator_ptr->Rs[WINDOW_SIZE];
    tmp_V = estimator_ptr->Vs[WINDOW_SIZE];
    tmp_Ba = estimator_ptr->Bas[WINDOW_SIZE];
    tmp_Bg = estimator_ptr->Bgs[WINDOW_SIZE];
    acc_0 = estimator_ptr->acc_0;
    gyr_0 = estimator_ptr->gyr_0;

    queue<sensor_msgs::ImuConstPtr> tmp_imu_buf = imu_buf;
    for (sensor_msgs::ImuConstPtr tmp_imu_msg; !tmp_imu_buf.empty(); tmp_imu_buf.pop())
        predict(tmp_imu_buf.front());

}

/**
 * @brief 同步一帧图像和多个IMU、GNSS观测的数据
 * 
 * @param[out] imu_msg      上一帧图像时间到当前帧图像时间的所有IMU数据 + 大于当前帧图像时间的第一帧IMU数据
 * @param[out] img_msg      图像特征数据
 * @param[out] gnss_msg     GNSS数据（与当前帧图像时间戳的时间差不大于0.05s）
 * @return true 
 * @return false 
 */
bool getMeasurements(std::vector<sensor_msgs::ImuConstPtr> &imu_msg, sensor_msgs::PointCloudConstPtr &img_msg, std::vector<ObsPtr> &gnss_msg)
{
    // 注意这个地方很有意思，是按照顺序进行或的，也就是如果imu不是空，这里就能过去。
    // 所以如果gnss一直收不到，那么也不影响单纯的VIO运行
    if (imu_buf.empty() || feature_buf.empty() || (GNSS_ENABLE && gnss_meas_buf.empty()))
        return false;
    
    double front_feature_ts = feature_buf.front()->header.stamp.toSec();

    // 最新的IMU时间比图像时间还早，说明imu还没到
    if (!(imu_buf.back()->header.stamp.toSec() > front_feature_ts))
    {
        //ROS_WARN("wait for imu, only should happen at the beginning");
        sum_of_wait++;
        return false;
    }
    double front_imu_ts = imu_buf.front()->header.stamp.toSec();

    // 最老的图像时间比最老的IMU时间还老，那么只能把图像丢掉
    while (!feature_buf.empty() && front_imu_ts > front_feature_ts)
    {
        ROS_WARN("throw img, only should happen at the beginning");
        feature_buf.pop();
        front_feature_ts = feature_buf.front()->header.stamp.toSec();
    }

    //; ----------- 至此，就找到了和IMU能够对齐的图像时间 

    if (GNSS_ENABLE)
    {
        front_feature_ts += time_diff_gnss_local;    // 补偿图像时间，和GNSS时间对齐
        double front_gnss_ts = time2sec(gnss_meas_buf.front()[0]->time);

        // 把太老的GNSS数据全部丢掉
        while (!gnss_meas_buf.empty() && front_gnss_ts < front_feature_ts-MAX_GNSS_CAMERA_DELAY)
        {
            ROS_WARN("throw gnss, only should happen at the beginning");
            gnss_meas_buf.pop();
            if (gnss_meas_buf.empty()) return false;
            front_gnss_ts = time2sec(gnss_meas_buf.front()[0]->time);
        }

        // 疑问：如果是在室内，此时GNSS全部失效，那这里返回false，岂不是VIO都不能运行了？
        if (gnss_meas_buf.empty())
        {
            ROS_WARN("wait for gnss...");
            return false;
        }
        // 如果在时间容忍范围内，则这个gnss观测数据就可以使用
        // 疑问：但是为什么还是用了一个时间容忍来寻找呢?
        else if (abs(front_gnss_ts-front_feature_ts) < MAX_GNSS_CAMERA_DELAY)
        {
            gnss_msg = gnss_meas_buf.front();
            gnss_meas_buf.pop();
        }
    }

    img_msg = feature_buf.front();
    feature_buf.pop();

    // 最后，把所有可用的IMU序列找出来（IMU时间戳小于相机时间戳+大于相机时间戳的第一帧IMU）
    while (imu_buf.front()->header.stamp.toSec() < img_msg->header.stamp.toSec() + estimator_ptr->td)   // estimator_ptr->td = 0.0
    {
        imu_msg.emplace_back(imu_buf.front());
        imu_buf.pop();
    }
    imu_msg.emplace_back(imu_buf.front());
    if (imu_msg.empty())
        ROS_WARN("no imu between two image");
    return true;
}

/**
 * @brief Imu消息存进imu_buf，同时按照imu频率（200Hz）预测predict位姿并发送(IMU状态递推并发布[P,Q,V,header])，提高里程计频率
 * 
 * @param imu_msg Imu消息的智能指针
 */
void imu_callback(const sensor_msgs::ImuConstPtr &imu_msg)
{
    if (imu_msg->header.stamp.toSec() <= last_imu_t)
    {
        ROS_WARN("imu message in disorder!");
        return;
    }
    last_imu_t = imu_msg->header.stamp.toSec();

    m_buf.lock();
    imu_buf.push(imu_msg);
    m_buf.unlock();
    con.notify_one();

    last_imu_t = imu_msg->header.stamp.toSec();

    {
        std::lock_guard<std::mutex> lg(m_state);
        predict(imu_msg);
        std_msgs::Header header = imu_msg->header;
        header.frame_id = "world";
        if (estimator_ptr->solver_flag == Estimator::SolverFlag::NON_LINEAR)
            pubLatestOdometry(tmp_P, tmp_Q, tmp_V, header);
    }
}

/**
 * @brief 订阅星历信息（GPS, Galileo, BeiDou）
 * 
 * @details 1.把ROS消息转成星历Ephem的数据结构
 *          2.把星历的数据结构存储到estimator的成员变量中
 * 
 * @param ephem_msg GnssEphemMsg消息的智能指针
 */
void gnss_ephem_callback(const GnssEphemMsgConstPtr &ephem_msg)
{
    EphemPtr ephem = msg2ephem(ephem_msg);  //将ROS星历信息，转换成相应的Ephem数据
    estimator_ptr->inputEphem(ephem);
}

/**
 * @brief 订阅星历信息（GLONASS）
 * 
 * @details 1.把ROS消息转成星历Ephem的数据结构
 *          2.把星历的数据结构存储到estimator的成员变量中
 * 
 * @param ephem_msg GnssGloEphemMsg消息的智能指针
 */
void gnss_glo_ephem_callback(const GnssGloEphemMsgConstPtr &glo_ephem_msg)
{
    GloEphemPtr glo_ephem = msg2glo_ephem(glo_ephem_msg);   //将ROS星历信息，转换成相应的Ephem数据
    estimator_ptr->inputEphem(glo_ephem);
}

void gnss_iono_params_callback(const StampedFloat64ArrayConstPtr &iono_msg)
{
    double ts = iono_msg->header.stamp.toSec();
    std::vector<double> iono_params;
    std::copy(iono_msg->data.begin(), iono_msg->data.end(), std::back_inserter(iono_params));
    assert(iono_params.size() == 8);
    estimator_ptr->inputIonoParams(ts, iono_params);
}

void gnss_meas_callback(const GnssMeasMsgConstPtr &meas_msg)
{
    std::vector<ObsPtr> gnss_meas = msg2meas(meas_msg);

    latest_gnss_time = time2sec(gnss_meas[0]->time);

    // cerr << "gnss ts is " << std::setprecision(20) << time2sec(gnss_meas[0]->time) << endl;
    if (!time_diff_valid)   return;

    m_buf.lock();
    gnss_meas_buf.push(std::move(gnss_meas));
    m_buf.unlock();
    con.notify_one();
}

/**
 * @brief feature回调函数，将feature_msg放入feature_buf
 * 
 * @param feature_msg 
 */
void feature_callback(const sensor_msgs::PointCloudConstPtr &feature_msg)
{
    ++ feature_msg_counter;

    if (skip_parameter < 0 && time_diff_valid)
    {
        const double this_feature_ts = feature_msg->header.stamp.toSec()+time_diff_gnss_local;
        
        // 如果已经接收到GNSS数据和图像特征数据，进行图像滤除（图像频率20Hz，GNSS频率10Hz）
        if (latest_gnss_time > 0 && tmp_last_feature_time > 0)
        {
            if (abs(this_feature_ts - latest_gnss_time) > abs(tmp_last_feature_time - latest_gnss_time))
                skip_parameter = feature_msg_counter%2;       // skip this frame and afterwards
            else
                skip_parameter = 1 - (feature_msg_counter%2);   // skip next frame and afterwards
        }

        // cerr << "feature counter is " << feature_msg_counter << ", skip parameter is " << int(skip_parameter) << endl;
        tmp_last_feature_time = this_feature_ts;
    }

    if (skip_parameter >= 0 && int(feature_msg_counter%2) != skip_parameter)
    {
        m_buf.lock();
        feature_buf.push(feature_msg);
        m_buf.unlock();
        con.notify_one();
    }
}

/**
 * @brief 订阅VI传感器的外部触发信息（时间硬同步）
 * 
 * @details trigger_msg记录的是VI传感器被GNSS脉冲触发时的本地时间，也可以理解成图像的命名（以本地时间命名）
 *          可以计算GNSS时间和本地时间的偏差，从而进行VI传感器的时间校正
 * 
 * @param trigger_msg 
 */
void local_trigger_info_callback(const gvins::LocalSensorExternalTriggerConstPtr &trigger_msg)
{
    std::lock_guard<std::mutex> lg(m_time);

    // 如果之前记录过PPS触发时间
    if (next_pulse_time_valid)
    {
        // next_pulse_time记录了PPS触发时的GPS时间
        // trigger_msg记录了VI传感器PPS触发时的本地时间
        // time_diff_gnss_local记录了时间改正数：PPS触发时，VI传感器的本地时间和GPS时间的差值
        time_diff_gnss_local = next_pulse_time - trigger_msg->header.stamp.toSec();
        estimator_ptr->inputGNSSTimeDiff(time_diff_gnss_local);
        if (!time_diff_valid)       // just get calibrated
            std::cout << "time difference between GNSS and VI-Sensor got calibrated: "
                << std::setprecision(15) << time_diff_gnss_local << " s\n";
        time_diff_valid = true;
    }
}

/**
 * @brief GNSS接收机的PPS触发信号的回调函数，内部存储PPS的触发时间
 * 
 * @param[in] tp_msg 
 */
void gnss_tp_info_callback(const GnssTimePulseInfoMsgConstPtr &tp_msg)
{
    // 先把GPS时间转成gtime_t数据结构
    gtime_t tp_time = gpst2time(tp_msg->time.week, tp_msg->time.tow);
    
    // 根据不同的卫星系统，将gps时间进一步处理
    if (tp_msg->utc_based || tp_msg->time_sys == SYS_GLO)
        tp_time = utc2gpst(tp_time);
    else if (tp_msg->time_sys == SYS_GAL)
        tp_time = gst2time(tp_msg->time.week, tp_msg->time.tow);
    else if (tp_msg->time_sys == SYS_BDS)
        tp_time = bdt2time(tp_msg->time.week, tp_msg->time.tow);
    else if (tp_msg->time_sys == SYS_NONE)
    {
        std::cerr << "Unknown time system in GNSSTimePulseInfoMsg.\n";
        return;
    }

    double gnss_ts = time2sec(tp_time);

    std::lock_guard<std::mutex> lg(m_time);
    next_pulse_time = gnss_ts;      // 记录PPS触发时间
    next_pulse_time_valid = true;   // 设置下一个pps时间有效
}

void restart_callback(const std_msgs::BoolConstPtr &restart_msg)
{
    if (restart_msg->data == true)
    {
        ROS_WARN("restart the estimator!");
        m_buf.lock();
        while(!feature_buf.empty())
            feature_buf.pop();
        while(!imu_buf.empty())
            imu_buf.pop();
        m_buf.unlock();
        m_estimator.lock();
        estimator_ptr->clearState();
        estimator_ptr->setParameter();
        m_estimator.unlock();
        current_time = -1;
        last_imu_t = 0;
    }
    return;
}

/**
 * @brief GVINS主程序，包含初始化，因子图优化
 */
void process()
{
    while (true)
    {
        std::vector<std::pair<std::vector<sensor_msgs::ImuConstPtr>, sensor_msgs::PointCloudConstPtr>> measurements;
        std::vector<sensor_msgs::ImuConstPtr> imu_msg;
        sensor_msgs::PointCloudConstPtr img_msg;    
        std::vector<ObsPtr> gnss_msg;               // gnss的观测信息，对于一个图像帧，会有多个卫星的观测，因此是vector

        // Step 1. 同步IMU、图像和GNSS数据
        std::unique_lock<std::mutex> lk(m_buf);
        con.wait(lk, [&]
                 {  // 这帧图像和上一帧图像之间包括：一帧图像特征点、多个IMU数据、多个gnss数据
                    return getMeasurements(imu_msg, img_msg, gnss_msg);
                 });
        lk.unlock();
        m_estimator.lock();

        // Step 2. 执行IMU预积分
        double dx = 0, dy = 0, dz = 0, rx = 0, ry = 0, rz = 0;
        for (auto &imu_data : imu_msg)
        {
            double t = imu_data->header.stamp.toSec();
            double img_t = img_msg->header.stamp.toSec() + estimator_ptr->td;   // estimator_ptr->td = 0.0
            if (t <= img_t)
            { 
                if (current_time < 0)
                    current_time = t;
                double dt = t - current_time;
                ROS_ASSERT(dt >= 0);
                current_time = t;
                dx = imu_data->linear_acceleration.x;
                dy = imu_data->linear_acceleration.y;
                dz = imu_data->linear_acceleration.z;
                rx = imu_data->angular_velocity.x;
                ry = imu_data->angular_velocity.y;
                rz = imu_data->angular_velocity.z;
                estimator_ptr->processIMU(dt, Vector3d(dx, dy, dz), Vector3d(rx, ry, rz));
                //printf("imu: dt:%f a: %f %f %f w: %f %f %f\n",dt, dx, dy, dz, rx, ry, rz);

            }
            else    // 针对最后一个imu数据，做一个简单的线性插值（插值到图像时间戳）
            {
                double dt_1 = img_t - current_time;
                double dt_2 = t - img_t;
                current_time = img_t;
                ROS_ASSERT(dt_1 >= 0);
                ROS_ASSERT(dt_2 >= 0);
                ROS_ASSERT(dt_1 + dt_2 > 0);
                double w1 = dt_2 / (dt_1 + dt_2);
                double w2 = dt_1 / (dt_1 + dt_2);
                dx = w1 * dx + w2 * imu_data->linear_acceleration.x;
                dy = w1 * dy + w2 * imu_data->linear_acceleration.y;
                dz = w1 * dz + w2 * imu_data->linear_acceleration.z;
                rx = w1 * rx + w2 * imu_data->angular_velocity.x;
                ry = w1 * ry + w2 * imu_data->angular_velocity.y;
                rz = w1 * rz + w2 * imu_data->angular_velocity.z;
                estimator_ptr->processIMU(dt_1, Vector3d(dx, dy, dz), Vector3d(rx, ry, rz));
                //printf("dimu: dt:%f a: %f %f %f w: %f %f %f\n",dt_1, dx, dy, dz, rx, ry, rz);
            }
        }

        // Step 3. 处理GNSS观测和星历信息，放到estimator的类成员变量中
        if (GNSS_ENABLE && !gnss_msg.empty())
            estimator_ptr->processGNSS(gnss_msg);

        ROS_DEBUG("processing vision data with stamp %f \n", img_msg->header.stamp.toSec());

        // Step 4. 统计前端的特征点追踪信息
        TicToc t_s;
        map<int, vector<pair<int, Eigen::Matrix<double, 7, 1>>>> image;
        for (unsigned int i = 0; i < img_msg->points.size(); i++)
        {
            int v = img_msg->channels[0].values[i] + 0.5;
            int feature_id = v / NUM_OF_CAM;
            int camera_id = v % NUM_OF_CAM;
            double x = img_msg->points[i].x;
            double y = img_msg->points[i].y;
            double z = img_msg->points[i].z;
            double p_u = img_msg->channels[1].values[i];
            double p_v = img_msg->channels[2].values[i];
            double velocity_x = img_msg->channels[3].values[i];
            double velocity_y = img_msg->channels[4].values[i];
            ROS_ASSERT(z == 1);
            Eigen::Matrix<double, 7, 1> xyz_uv_velocity;
            xyz_uv_velocity << x, y, z, p_u, p_v, velocity_x, velocity_y;
            image[feature_id].emplace_back(camera_id,  xyz_uv_velocity);
        }

        // Step 5. 重点：后端优化
        estimator_ptr->processImage(image, img_msg->header);

        // Step 6. 一次处理完成，进行一些统计信息计算
        double whole_t = t_s.toc();
        printStatistics(*estimator_ptr, whole_t);
        std_msgs::Header header = img_msg->header;
        header.frame_id = "world";

        pubOdometry(*estimator_ptr, header);
        pubKeyPoses(*estimator_ptr, header);
        pubCameraPose(*estimator_ptr, header);
        pubPointCloud(*estimator_ptr, header);
        pubTF(*estimator_ptr, header);
        pubKeyframe(*estimator_ptr);
        m_estimator.unlock();
        m_buf.lock();
        m_state.lock();
        if (estimator_ptr->solver_flag == Estimator::SolverFlag::NON_LINEAR)
            update();
        m_state.unlock();
        m_buf.unlock();
    }
}

int main(int argc, char **argv)
{
    ros::init(argc, argv, "gvins");
    ros::NodeHandle n("~");
    ros::console::set_logger_level(ROSCONSOLE_DEFAULT_NAME, ros::console::levels::Info);
    readParameters(n);
    estimator_ptr.reset(new Estimator());
    estimator_ptr->setParameter();
#ifdef EIGEN_DONT_PARALLELIZE
    ROS_DEBUG("EIGEN_DONT_PARALLELIZE");
#endif

    registerPub(n);

    next_pulse_time_valid = false;
    time_diff_valid = false;
    latest_gnss_time = -1;
    tmp_last_feature_time = -1;
    feature_msg_counter = 0;

    if (GNSS_ENABLE)
        skip_parameter = -1;
    else
        skip_parameter = 0;

    ros::Subscriber sub_imu = n.subscribe(IMU_TOPIC, 2000, imu_callback, ros::TransportHints().tcpNoDelay());
    ros::Subscriber sub_feature = n.subscribe("/gvins_feature_tracker/feature", 2000, feature_callback);
    ros::Subscriber sub_restart = n.subscribe("/gvins_feature_tracker/restart", 2000, restart_callback);

    ros::Subscriber sub_ephem, sub_glo_ephem, sub_gnss_meas, sub_gnss_iono_params;
    ros::Subscriber sub_gnss_time_pluse_info, sub_local_trigger_info;
    
    // GNSS相关
    if (GNSS_ENABLE)
    {
        // 1.订阅星历信息：卫星的位置、速度、时间偏差等信息
        sub_ephem = n.subscribe(GNSS_EPHEM_TOPIC, 100, gnss_ephem_callback);                        //GPS, Galileo, BeiDou ephemeris
        sub_glo_ephem = n.subscribe(GNSS_GLO_EPHEM_TOPIC, 100, gnss_glo_ephem_callback);            //GLONASS ephemeris

        // 2.订阅卫星的观测信息
        sub_gnss_meas = n.subscribe(GNSS_MEAS_TOPIC, 100, gnss_meas_callback);                      //GNSS raw measurement topic
        
        // 3.订阅电离层延时相关信息
        sub_gnss_iono_params = n.subscribe(GNSS_IONO_PARAMS_TOPIC, 100, gnss_iono_params_callback); //GNSS broadcast ionospheric parameters

        if (GNSS_LOCAL_ONLINE_SYNC)
        {
            sub_gnss_time_pluse_info = n.subscribe(GNSS_TP_INFO_TOPIC, 100, 
                gnss_tp_info_callback);
            sub_local_trigger_info = n.subscribe(LOCAL_TRIGGER_INFO_TOPIC, 100, 
                local_trigger_info_callback);
        }
        else
        {
            time_diff_gnss_local = GNSS_LOCAL_TIME_DIFF;
            estimator_ptr->inputGNSSTimeDiff(time_diff_gnss_local);
            time_diff_valid = true;
        }
    }

    std::thread measurement_process{process};
    ros::spin();

    return 0;
}
