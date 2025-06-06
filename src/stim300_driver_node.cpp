#include "driver_stim300.h"
#include "serial_unix.h"
#include "math.h"
#include "ros/ros.h"
#include "sensor_msgs/Imu.h"
#include "std_srvs/Trigger.h"
#include "std_srvs/Empty.h"
#include "iostream"
#include <Eigen/Dense>
#include <vector>

bool calibration_mode{false};
constexpr int NUMBER_OF_CALIBRATION_SAMPLES{500};
constexpr double ACC_TOLERANCE{0.1};
constexpr double MAX_DROPPED_ACC_X_MSG{5};
constexpr double MAX_DROPPED_ACC_Y_MSG{5};
constexpr double MAX_DROPPED_ACC_Z_MSG{5};
constexpr double MAX_DROPPED_GYRO_X_MSG{5};
constexpr double MAX_DROPPED_GYRO_Y_MSG{5};
constexpr double MAX_DROPPED_GYRO_Z_MSG{5};
constexpr double GYRO_X_PEAK_TO_PEAK_NOISE{0.002};
constexpr double GYRO_Y_PEAK_TO_PEAK_NOISE{0.002};
constexpr double GYRO_Z_PEAK_TO_PEAK_NOISE{0.002};
// for covariance calculation
std::vector<Eigen::Vector3d> acc_cal_samples;
std::vector<Eigen::Vector3d> gyro_cal_samples;
//for calibration
int number_of_samples{0};

// helper: unbiased covariance
Eigen::Matrix3d computeCovariance(const std::vector<Eigen::Vector3d>& data) {
  const int N = data.size();
  Eigen::Vector3d mean = Eigen::Vector3d::Zero();
  for (auto& v : data) mean += v;
  mean /= double(N);

  Eigen::Matrix3d cov = Eigen::Matrix3d::Zero();
  for (auto& v : data) {
    Eigen::Vector3d d = v - mean;
    cov += d * d.transpose();
  }
  return cov / double(N - 1);
}

struct Quaternion
{
    double w, x, y, z;
};

struct EulerAngles {
    double roll, pitch, yaw;
};

Quaternion FromRPYToQuaternion(EulerAngles angles) // yaw (Z), pitch (Y), roll (X)
{
    // Abbreviations for the various angular functions
    double cy = cos(angles.yaw * 0.5);
    double sy = sin(angles.yaw * 0.5);
    double cp = cos(angles.pitch * 0.5);
    double sp = sin(angles.pitch * 0.5);
    double cr = cos(angles.roll * 0.5);
    double sr = sin(angles.roll * 0.5);

    Quaternion q;
    q.w = cy * cp * cr + sy * sp * sr;
    q.x = cy * cp * sr - sy * sp * cr;
    q.y = sy * cp * sr + cy * sp * cr;
    q.z = sy * cp * cr - cy * sp * sr;

    return q;
}


bool responseCalibrateIMU(std_srvs::Trigger::Request &calibration_request, std_srvs::Trigger::Response &calibration_response)
{

    if (calibration_mode == false)
    {
        calibration_mode = true;
        acc_cal_samples.clear();
        gyro_cal_samples.clear();
        number_of_samples = 0;
        calibration_response.message = "IMU in calibration mode ";
        calibration_response.success = true;
    }
    
    return true;
}


int main(int argc, char** argv)
{
  ros::init(argc, argv, "stim300_driver_node");

  ros::NodeHandle node;
  ros::NodeHandle pnh("~");    // for private params

  std::string device_name;
  std::string frame_id;
  double variance_gyro{0};
  double variance_acc{0};
  int sample_rate{0};
  double gravity{0};
  bool calibrate_on_start{false};
  double roll_offset{0}, pitch_offset{0};
  pnh.param<std::string>("device_name", device_name, "/dev/ttyUSB0");
  pnh.param<std::string>("frame_id", frame_id, "imu_0");
  pnh.param("variance_gyro", variance_gyro, 0.0001 * 2 * 4.6 * pow(10, -4));
  pnh.param("variance_acc", variance_acc, 0.000055);
  pnh.param("sample_rate", sample_rate, 125);
  pnh.param("gravity", gravity, 9.80665);
  pnh.param("calibrate_on_start", calibrate_on_start, false);
  pnh.param("roll_offset", roll_offset, 0.0);
  pnh.param("pitch_offset", pitch_offset, 0.0);




  // These values have been estimated by having beluga in a pool for a couple of minutes, and then calculate the variance for each values
  sensor_msgs::Imu stim300msg{};
  stim300msg.angular_velocity_covariance[0] = 0.0000027474;
  stim300msg.angular_velocity_covariance[4] = 0.0000027474;
  stim300msg.angular_velocity_covariance[8] = 0.000007312;
  stim300msg.linear_acceleration_covariance[0] = 0.00041915;
  stim300msg.linear_acceleration_covariance[4] = 0.00041915;
  stim300msg.linear_acceleration_covariance[8] = 0.000018995;
  // stim300msg.orientation.x = 0.00000024358;
  // stim300msg.orientation.y = 0.00000024358;
  // stim300msg.orientation.z = 0.00000024358;
  stim300msg.orientation.x = 0.0;
  stim300msg.orientation.y = 0.0;
  stim300msg.orientation.z = 0.0;
  stim300msg.header.frame_id = frame_id;

if (calibrate_on_start) {
  ROS_INFO("Auto-calibration requested at startup, entering calibration mode...");
  calibration_mode = true;
}

  ros::Publisher imuSensorPublisher = node.advertise<sensor_msgs::Imu>("imu/data_raw", 1000);
  //ros::Publisher orientationPublisher = node.advertise<sensor_msgs::Imu>("imu/orientation", 1000);
  ros::ServiceServer service = node.advertiseService("IMU_calibration",responseCalibrateIMU);


  // New messages are sent from the sensor with sample_rate
  // As loop_rate determines how often we check for new data
  // on the serial buffer, theoretically loop_rate = sample_rate
  // should be okey, but to be sure we double it
  ros::Rate loop_rate(sample_rate * 2);

  try {
    SerialUnix serial_driver(device_name, stim_const::BaudRate::BAUD_460800);
    DriverStim300 driver_stim300(serial_driver);

    ROS_INFO("STIM300 IMU driver initialized successfully");

    int difference_in_dataGram{0};
    int count_messages{0};
    double inclination_x{0};
    double inclination_y{0};
    double inclination_z{0};

    double average_calibration_roll{0};
    double average_calibration_pitch{0};
    double inclination_x_calibration_sum{0};
    double inclination_y_calibration_sum{0};
    double inclination_z_calibration_sum{0};
    double inclination_x_average{0};
    double inclination_y_average{0};
    double inclination_z_average{0};

    // Acc wild point filter
    std::vector<double> acceleration_buffer_x{};
    std::vector<double> acceleration_buffer_y{};
    std::vector<double> acceleration_buffer_z{};
    double dropped_acceleration_x_msg{0.0};
    double dropped_acceleration_y_msg{0.0};
    double dropped_acceleration_z_msg{0.0};

    // Gyro wild point filter
    std::vector<double> gyro_buffer_x{};
    std::vector<double> gyro_buffer_y{};
    std::vector<double> gyro_buffer_z{};
    double dropped_gyro_x_msg{0.0};
    double dropped_gyro_y_msg{0.0};
    double dropped_gyro_z_msg{0.0};

  


    while (ros::ok())
    {
      switch (driver_stim300.update())
      {
        case Stim300Status::NORMAL:
          break;
        case Stim300Status::OUTSIDE_OPERATING_CONDITIONS:
          ROS_DEBUG("Stim 300 outside operating conditions");
          break;
        case Stim300Status::NEW_MEASURMENT:
              inclination_x = driver_stim300.getIncX();
              inclination_y = driver_stim300.getIncY();
              inclination_z = driver_stim300.getIncZ();
              Quaternion q;
              EulerAngles RPY;
           if (calibration_mode == true)
            {
              // std::cout<<"in calibration_mode"<<std::endl;
                if(number_of_samples < NUMBER_OF_CALIBRATION_SAMPLES)
                {
                    // std::cout<<"in calibration_mode"<<std::endl;
                    number_of_samples++;
                    // std::cout<<"num of samples" << number_of_samples<<std::endl;
                    inclination_x_calibration_sum += inclination_x;
                    inclination_y_calibration_sum += inclination_y;
                    inclination_z_calibration_sum += inclination_z;
                  // store raw (m/s²) and gyro (rad/s)
                    acc_cal_samples.emplace_back(
                      driver_stim300.getAccX() * gravity,
                      driver_stim300.getAccY() * gravity,
                      driver_stim300.getAccZ() * gravity
                    );
                    gyro_cal_samples.emplace_back(
                      driver_stim300.getGyroX(),
                      driver_stim300.getGyroY(),
                      driver_stim300.getGyroZ()
                    );
                }
                else
                {
                    // std::cout<<"in else"<<std::endl;
                    inclination_x_average = inclination_x_calibration_sum/NUMBER_OF_CALIBRATION_SAMPLES;
                    inclination_y_average = inclination_y_calibration_sum/NUMBER_OF_CALIBRATION_SAMPLES;
                    inclination_z_average = inclination_z_calibration_sum/NUMBER_OF_CALIBRATION_SAMPLES;

                    average_calibration_roll = atan2(inclination_y_average,inclination_z_average);
                    average_calibration_pitch = atan2(-inclination_x_average,sqrt(pow(inclination_y_average,2)+pow(inclination_z_average,2)));
                    // std::cout<<average_calibration_roll<<std::endl;
                    // std::cout<<average_calibration_pitch<<std::endl;
                    ROS_INFO("STIM300 IMU Calibrated");
                    ROS_INFO("STIM300 average calibration ROLL offset: %f", average_calibration_roll);
                    ROS_INFO("STIM300 average calibration PITCH offset: %f", average_calibration_pitch);
                    roll_offset  = average_calibration_roll;
                    pitch_offset = average_calibration_pitch;
                    pnh.setParam("roll_offset",  roll_offset);
                    pnh.setParam("pitch_offset", pitch_offset);
                    number_of_samples = 0;
                    inclination_x_calibration_sum = 0.0;
                    inclination_y_calibration_sum = 0.0;
                    inclination_z_calibration_sum = 0.0;
                  // now compute covariances:
                    Eigen::Matrix3d acc_cov  = computeCovariance(acc_cal_samples);
                    Eigen::Matrix3d gyro_cov = computeCovariance(gyro_cal_samples);

                    // inject into your IMU message template:
                    stim300msg.linear_acceleration_covariance[0] = acc_cov(0,0);
                    stim300msg.linear_acceleration_covariance[1] = acc_cov(0,1);
                    stim300msg.linear_acceleration_covariance[2] = acc_cov(0,2);
                    stim300msg.linear_acceleration_covariance[3] = acc_cov(1,0);
                    stim300msg.linear_acceleration_covariance[4] = acc_cov(1,1);
                    stim300msg.linear_acceleration_covariance[5] = acc_cov(1,2);
                    stim300msg.linear_acceleration_covariance[6] = acc_cov(2,0);
                    stim300msg.linear_acceleration_covariance[7] = acc_cov(2,1);
                    stim300msg.linear_acceleration_covariance[8] = acc_cov(2,2);

                    stim300msg.angular_velocity_covariance[0] = gyro_cov(0,0);
                    stim300msg.angular_velocity_covariance[1] = gyro_cov(0,1);
                    stim300msg.angular_velocity_covariance[2] = gyro_cov(0,2);
                    stim300msg.angular_velocity_covariance[3] = gyro_cov(1,0);
                    stim300msg.angular_velocity_covariance[4] = gyro_cov(1,1);
                    stim300msg.angular_velocity_covariance[5] = gyro_cov(1,2);
                    stim300msg.angular_velocity_covariance[6] = gyro_cov(2,0);
                    stim300msg.angular_velocity_covariance[7] = gyro_cov(2,1);
                    stim300msg.angular_velocity_covariance[8] = gyro_cov(2,2);

                    ROS_INFO("Computed covariances: Acc[0,0]=%.6f Gyro[0,0]=%.6f",
                            acc_cov(0,0), gyro_cov(0,0));
                    calibration_mode = false;
                }
              break;  
            }
            else
            {
                    // RPY.roll = atan2(inclination_y,inclination_z);
                    // RPY.pitch = atan2(-inclination_x,sqrt(pow(inclination_y,2)+pow(inclination_z,2)));
                    // q = FromRPYToQuaternion(RPY);
                    double raw_roll  = atan2(inclination_y, inclination_z) - roll_offset;
                    double raw_pitch = atan2(-inclination_x,sqrt(pow(inclination_y,2)+pow(inclination_z,2))) - pitch_offset;
                    RPY.roll  = raw_roll;
                    RPY.pitch = raw_pitch;
                    RPY.yaw   = 0.0;

                    q = FromRPYToQuaternion(RPY);

                    // Acceleration wild point filter

                    // Previous message
                    acceleration_buffer_x.push_back(driver_stim300.getAccX() * gravity);
                    acceleration_buffer_y.push_back(driver_stim300.getAccY() * gravity);
                    acceleration_buffer_z.push_back(driver_stim300.getAccZ() * gravity);
                    stim300msg.header.stamp = ros::Time::now();

                    if (acceleration_buffer_x.size() == 2 && acceleration_buffer_y.size() == 2 && acceleration_buffer_z.size() == 2)
                    {
                      if(std::abs(acceleration_buffer_x.back() - acceleration_buffer_x.front()) < ACC_TOLERANCE || dropped_acceleration_x_msg > MAX_DROPPED_ACC_X_MSG )
                      {
                        stim300msg.linear_acceleration.x = acceleration_buffer_x.back();
                        dropped_acceleration_x_msg = 0;
                      }
                      else
                      {
                        ROS_DEBUG("ACC_X_MSG wild point rejected");
                        dropped_acceleration_x_msg +=1;
                      }

                      if(std::abs(acceleration_buffer_y.back() - acceleration_buffer_y.front()) < ACC_TOLERANCE || dropped_acceleration_y_msg > MAX_DROPPED_ACC_Y_MSG )
                      {
                        stim300msg.linear_acceleration.y = acceleration_buffer_y.back();
                        dropped_acceleration_y_msg = 0;
                      }
                      else
                      {
                        ROS_DEBUG("ACC_Y_MSG wild point rejected");
                        dropped_acceleration_y_msg +=1;
                      }

                      if(std::abs(acceleration_buffer_z.back() - acceleration_buffer_z.front()) < ACC_TOLERANCE || dropped_acceleration_z_msg > MAX_DROPPED_ACC_Z_MSG )
                      {
                        stim300msg.linear_acceleration.z = acceleration_buffer_z.back();
                        dropped_acceleration_z_msg = 0;
                      }
                      else
                      {
                        ROS_DEBUG("ACC_Z_MSG wild point rejected");
                        dropped_acceleration_z_msg +=1;
                      }
                      // Empty acceleration buffers
                      acceleration_buffer_x = std::vector<double>{acceleration_buffer_x.back()};
                      acceleration_buffer_y = std::vector<double>{acceleration_buffer_y.back()};
                      acceleration_buffer_z = std::vector<double>{acceleration_buffer_z.back()};
                    }
                    else
                    {
                      stim300msg.linear_acceleration.x = driver_stim300.getAccX() * gravity;
                      stim300msg.linear_acceleration.y = driver_stim300.getAccY() * gravity;
                      stim300msg.linear_acceleration.z = driver_stim300.getAccZ() * gravity;
                    }

                    // Gyro wild point filter
                    gyro_buffer_x.push_back(driver_stim300.getGyroX());
                    gyro_buffer_y.push_back(driver_stim300.getGyroY());
                    gyro_buffer_z.push_back(driver_stim300.getGyroZ());

                    if(gyro_buffer_x.size() == 2 && gyro_buffer_y.size() == 2 && gyro_buffer_z.size() == 2)
                    {

                      if(std::abs(gyro_buffer_x.back() - gyro_buffer_x.front()) < std::max(2*std::abs(gyro_buffer_x.front()),GYRO_X_PEAK_TO_PEAK_NOISE) || dropped_gyro_x_msg > MAX_DROPPED_GYRO_X_MSG)
                      {
                        stim300msg.angular_velocity.x = gyro_buffer_x.back();
                        dropped_gyro_x_msg = 0;
                      }
                      else
                      {
                        ROS_DEBUG("GYRO_X_MSG wild point rejected");
                        dropped_gyro_x_msg += 1;
                      }

                      if(std::abs(gyro_buffer_y.back() - gyro_buffer_y.front()) < std::max(2*std::abs(gyro_buffer_y.front()),GYRO_Y_PEAK_TO_PEAK_NOISE) || dropped_gyro_y_msg > MAX_DROPPED_GYRO_Y_MSG)
                      {
                        stim300msg.angular_velocity.x = gyro_buffer_y.back();
                        dropped_gyro_y_msg = 0;
                      }
                      else
                      {
                        ROS_DEBUG("GYRO_Y_MSG wild point rejected");
                        dropped_gyro_y_msg += 1;
                      }

                      if(std::abs(gyro_buffer_z.back() - gyro_buffer_z.front()) < std::max(2*std::abs(gyro_buffer_z.front()),GYRO_Z_PEAK_TO_PEAK_NOISE) || dropped_gyro_z_msg > MAX_DROPPED_GYRO_Z_MSG)
                      {
                        stim300msg.angular_velocity.z = gyro_buffer_z.back();
                        dropped_gyro_z_msg = 0;
                      }
                      else
                      {
                        ROS_DEBUG("GYRO_Z_MSG wild point rejected");
                        dropped_gyro_z_msg += 1;
                      }

                      // Empty buffers
                      gyro_buffer_x = std::vector<double>{gyro_buffer_x.back()};
                      gyro_buffer_y = std::vector<double>{gyro_buffer_y.back()};
                      gyro_buffer_z = std::vector<double>{gyro_buffer_z.back()};
                    }
                    else
                    {
                      stim300msg.angular_velocity.x = driver_stim300.getGyroX();
                      stim300msg.angular_velocity.y = driver_stim300.getGyroY();
                      stim300msg.angular_velocity.z = driver_stim300.getGyroZ();
                    }
                    stim300msg.orientation.w = q.w;
                    stim300msg.orientation.x = q.x;
                    stim300msg.orientation.y = q.y;
                    stim300msg.orientation.z = q.z;
                    imuSensorPublisher.publish(stim300msg);
                    break;
            }
        case Stim300Status::CONFIG_CHANGED:
          ROS_INFO("Updated Stim 300 imu config: ");
          ROS_INFO("%s", driver_stim300.printSensorConfig().c_str());
          //loop_rate = driver_stim300.getSampleRate()*2; // WRONG: assigning an int to ros::Rate
          loop_rate = ros::Rate(driver_stim300.getSampleRate() * 2);
          break;
        case Stim300Status::STARTING_SENSOR:
          ROS_INFO("Stim 300 IMU is warming up.");
          break;
        case Stim300Status::SYSTEM_INTEGRITY_ERROR:
          ROS_WARN("Stim 300 IMU system integrity error.");
          break;
        case Stim300Status::OVERLOAD:
          ROS_WARN("Stim 300 IMU overload.");
          break;
        case Stim300Status::ERROR_IN_MEASUREMENT_CHANNEL:
          ROS_WARN("Stim 300 IMU error in measurement channel.");
          break;
        case Stim300Status::ERROR:
          ROS_WARN("Stim 300 IMU: internal error.");

      }

      loop_rate.sleep();
      ros::spinOnce();
    }
    return 0;
  } catch (std::runtime_error &error) {
    // TODO: Reset IMU
    ROS_ERROR("%s\n", error.what());
    return 0;
  }
}