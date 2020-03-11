#include <chrono>
#include <memory>

#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>
#include <geometry_msgs/msg/twist.hpp>
#include <geometry_msgs/msg/quaternion.hpp>
#include <sensor_msgs/msg/laser_scan.hpp>
#include <cmath>
#include <epuck_ros2_cpp/i2c_wrapper.hpp>

extern "C"
{
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <unistd.h>
}

#define MSG_ACTUATORS_SIZE 20
#define MSG_SENSORS_SIZE 47
#define PERIOD_MS 64

#define min(X, Y) (((X) < (Y)) ? (X) : (Y))
#define max(X, Y) (((X) > (Y)) ? (X) : (Y))
#define CLIP(VAL, MIN, MAX) max(min((MAX), (VAL)), (MIN))

using namespace std::chrono_literals;

const double WHEEL_DISTANCE = 0.05685;
const double WHEEL_RADIUS = 0.02;

class EPuckPublisher : public rclcpp::Node
{
public:
  EPuckPublisher()
      : Node("pipuck_driver")
  {
    i2c_main = I2CWrapperTest("/dev/i2c-4");

    memset(msg_actuators, 0, MSG_ACTUATORS_SIZE);
    memset(msg_sensors, 0, MSG_SENSORS_SIZE);

    subscription = this->create_subscription<geometry_msgs::msg::Twist>("cmd_vel", 1, std::bind(&EPuckPublisher::on_cmd_vel_received, this, std::placeholders::_1));
    laser_publisher = this->create_publisher<sensor_msgs::msg::LaserScan>("laser", 1);

    timer = this->create_wall_timer(
        std::chrono::milliseconds(PERIOD_MS), std::bind(&EPuckPublisher::update_callback, this));

    RCLCPP_INFO(this->get_logger(), "EPuck Driver has been initialized");
  }

  ~EPuckPublisher()
  {
    close(fh);
  }

private:
  static float intensity_to_distance(float p_x)
  {
    std::vector<std::vector<float>> table = {
        {0, 4095},
        {0.005, 2133.33},
        {0.01, 1465.73},
        {0.015, 601.46},
        {0.02, 383.84},
        {0.03, 234.93},
        {0.04, 158.03},
        {0.05, 120},
        {0.06, 104.09},
        {0.07, 67.19},
        {0.1, 0.0}};
    for (int i = 0; i < table.size() - 1; i++)
    {
      if (table[i][1] >= p_x and table[i + 1][1] < p_x)
      {
        float b_x = table[i][1];
        float b_y = table[i][0];
        float a_x = table[i + 1][1];
        float a_y = table[i + 1][0];
        float p_y = ((b_y - a_y) / (b_x - a_x)) * (p_x - a_x) + a_y;
        return p_y;
      }
    }
    return 100.0;
  }

  static geometry_msgs::msg::Quaternion::SharedPtr euler_to_quaternion(double roll, double pitch, double yaw)
  {
    geometry_msgs::msg::Quaternion::SharedPtr q;
    q->x = sin(roll / 2) * cos(pitch / 2) * cos(yaw / 2) - cos(roll / 2) * sin(pitch / 2) * sin(yaw / 2);
    q->y = cos(roll / 2) * sin(pitch / 2) * cos(yaw / 2) + sin(roll / 2) * cos(pitch / 2) * sin(yaw / 2);
    q->z = cos(roll / 2) * cos(pitch / 2) * sin(yaw / 2) - sin(roll / 2) * sin(pitch / 2) * cos(yaw / 2);
    q->w = cos(roll / 2) * cos(pitch / 2) * cos(yaw / 2) + sin(roll / 2) * sin(pitch / 2) * sin(yaw / 2);
    return q;
  }

  void on_cmd_vel_received(const geometry_msgs::msg::Twist::SharedPtr msg)
  {
    double left_velocity = (2.0 * msg->linear.x - msg->angular.z * WHEEL_DISTANCE) / (2.0 * WHEEL_RADIUS);
    double right_velocity = (2.0 * msg->linear.x + msg->angular.z * WHEEL_DISTANCE) / (2.0 * WHEEL_RADIUS);

    int left_velocity_big = CLIP(left_velocity / 0.0068, -1108, 1108);
    int right_velocity_big = CLIP(right_velocity / 0.0068, -1108, 1108);

    RCLCPP_INFO(this->get_logger(), "New velocity, left %d and right %d", left_velocity_big, left_velocity_big);

    msg_actuators[0] = left_velocity_big & 0xFF;
    msg_actuators[1] = (left_velocity_big >> 8) & 0xFF;
    msg_actuators[2] = right_velocity_big & 0xFF;
    msg_actuators[3] = (right_velocity_big >> 8) & 0xFF;
  }

  void publish_distance_data(rclcpp::Time &stamp)
  {
    const float distance_from_center = 0.035;
    float dist[8];
    auto msg = sensor_msgs::msg::LaserScan();

    // Decode measurements
    for (int i = 0; i < 8; i++)
    {
      int distance_intensity = msg_sensors[i * 2] + (msg_sensors[i * 2 + 1] << 8);
      float distance = EPuckPublisher::intensity_to_distance(distance_intensity) + distance_from_center;
      dist[i] = distance;
    }

    // Create message
    msg.header.frame_id = "laser_scanner";
    msg.header.stamp = stamp;
    msg.angle_min = -150 * M_PI / 180;
    msg.angle_max = 150 * M_PI / 180;
    msg.angle_increment = 15 * M_PI / 180.0;
    msg.scan_time = PERIOD_MS / 1000;
    msg.range_min = 0.005 + distance_from_center;
    msg.range_max = 0.05 + distance_from_center;
    msg.ranges = std::vector<float>{
        dist[4],                               // -150
        (3 / 4) * dist[4] + (1 / 4) * dist[5], // -135
        (2 / 4) * dist[4] + (2 / 4) * dist[5], // -120
        (1 / 4) * dist[4] + (3 / 4) * dist[5], // -105
        dist[5],                               // -90
        (2 / 3) * dist[5] + (1 / 3) * dist[6], // -75
        (1 / 3) * dist[5] + (2 / 3) * dist[6], // -60
        dist[6],                               // -45
        (1 / 2) * dist[6] + (1 / 2) * dist[7], // -30
        dist[7],                               // -15
        (1 / 2) * dist[7] + (1 / 2) * dist[0], // dist['tof'],                          // 0
        dist[0],                               // 15
        (1 / 2) * dist[0] + (1 / 2) * dist[1], // 30
        dist[1],                               // 45
        (2 / 3) * dist[1] + (1 / 3) * dist[2], // 60
        (1 / 3) * dist[1] + (2 / 3) * dist[2], // 75
        dist[2],                               // 90
        (3 / 4) * dist[2] + (1 / 4) * dist[3], // 105
        (2 / 4) * dist[2] + (2 / 4) * dist[3], // 120
        (1 / 4) * dist[2] + (3 / 4) * dist[3], // 135
        dist[3],                               // 150
    };

    // Publish the message
    laser_publisher->publish(msg);
  }

  void update_callback()
  {
    int status;
    rclcpp::Time stamp;

    // Main MCU
    status = i2c_main.set_address(0x1F);
    assert(status >= 0);

    // Main MCU: Write
    for (int i = 0; i < MSG_ACTUATORS_SIZE - 1; i++)
    {
      // msg_actuators[MSG_ACTUATORS_SIZE - 1] ^= msg_actuators[i];
    }

    i2c_main.write_data(msg_actuators, MSG_ACTUATORS_SIZE);
    i2c_main.read_data(msg_sensors, MSG_SENSORS_SIZE);
    if (status != MSG_SENSORS_SIZE)

      stamp = now();
  }

  rclcpp::TimerBase::SharedPtr timer;
  rclcpp::Publisher<sensor_msgs::msg::LaserScan>::SharedPtr laser_publisher;
  rclcpp::Subscription<geometry_msgs::msg::Twist>::SharedPtr subscription;

  I2CWrapperTest i2c_main;

  int fh;
  char msg_actuators[MSG_ACTUATORS_SIZE];
  char msg_sensors[MSG_SENSORS_SIZE];
};

int main(int argc, char *argv[])
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<EPuckPublisher>());
  rclcpp::shutdown();
  return 0;
}