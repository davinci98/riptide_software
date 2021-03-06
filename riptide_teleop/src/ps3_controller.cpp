#include "riptide_teleop/ps3_controller.h"

#define GRAVITY 9.81       // [m/s^2]
#define WATER_DENSITY 1000 // [kg/m^3]

bool IS_ATTITUDE_RESET = false;
bool IS_DEPTH_RESET = false;

int main(int argc, char **argv)
{
  ros::init(argc, argv, "ps3_controller");
  PS3Controller ps3;
  ps3.Loop();
}

PS3Controller::PS3Controller() : nh("ps3_controller")
{
  joy_sub = nh.subscribe<sensor_msgs::Joy>("/joy", 1, &PS3Controller::JoyCB, this);
  depth_sub = nh.subscribe<riptide_msgs::Depth>("/state/depth", 1, &PS3Controller::DepthCB, this);
  imu_sub = nh.subscribe<riptide_msgs::Imu>("/state/imu", 1, &PS3Controller::ImuCB, this);
  roll_pub = nh.advertise<riptide_msgs::AttitudeCommand>("/command/roll", 1);
  pitch_pub = nh.advertise<riptide_msgs::AttitudeCommand>("/command/pitch", 1);
  yaw_pub = nh.advertise<riptide_msgs::AttitudeCommand>("/command/yaw", 1);
  moment_pub = nh.advertise<geometry_msgs::Vector3Stamped>("/command/moment", 1);
  x_force_pub = nh.advertise<std_msgs::Float64>("/command/force_x", 1);
  y_force_pub = nh.advertise<std_msgs::Float64>("/command/force_y", 1);
  z_force_pub = nh.advertise<std_msgs::Float64>("/command/force_z", 1);
  depth_pub = nh.advertise<riptide_msgs::DepthCommand>("/command/depth", 1);
  reset_pub = nh.advertise<riptide_msgs::ResetControls>("/controls/reset", 1);
  plane_pub = nh.advertise<std_msgs::Int8>("/command/ps3_plane", 1);
  pneumatics_pub = nh.advertise<riptide_msgs::Pneumatics>("/command/pneumatics", 1);

  PS3Controller::LoadParam<bool>("enable_depth", enableDepth);        // Is depth sensor working?
  PS3Controller::LoadParam<bool>("enable_attitude", enableAttitude);  // Is IMU sensor working?
  PS3Controller::LoadParam<double>("rate", rt);                       // [Hz]
  PS3Controller::LoadParam<double>("max_roll_limit", MAX_ROLL);       // [m/s^2]
  PS3Controller::LoadParam<double>("max_pitch_limit", MAX_PITCH);     // [m/s^2]
  PS3Controller::LoadParam<double>("max_x_force", MAX_X_FORCE);       // [m/s^2]
  PS3Controller::LoadParam<double>("max_y_force", MAX_Y_FORCE);       // [m/s^2]
  PS3Controller::LoadParam<double>("max_z_force", MAX_Z_FORCE);       // [m/s^2]
  PS3Controller::LoadParam<double>("max_x_moment", MAX_X_MOMENT);     // [rad/s^2]
  PS3Controller::LoadParam<double>("max_y_moment", MAX_Y_MOMENT);     // [rad/s^2]
  PS3Controller::LoadParam<double>("max_z_moment", MAX_Z_MOMENT);     // [rad/s^2]
  PS3Controller::LoadParam<double>("max_depth", MAX_DEPTH);           // [m]
  PS3Controller::LoadParam<double>("cmd_roll_rate", CMD_ROLL_RATE);   // [deg/s]
  PS3Controller::LoadParam<double>("cmd_pitch_rate", CMD_PITCH_RATE); // [deg/s]
  PS3Controller::LoadParam<double>("cmd_yaw_rate", CMD_YAW_RATE);     // [deg/s]
  PS3Controller::LoadParam<double>("cmd_depth_rate", CMD_DEPTH_RATE); // [deg/s]
  PS3Controller::LoadParam<double>("boost", boost);                   // Factor to multiply pressed button/axis for faster rate
  // When using the boost factor, you must subtract one from its value for it
  // to have the desired effect. See below for implementation

  isReset = true;
  isStarted = false;
  isR2Init = false;
  isL2Init = false;
  isDepthInit = false;

  current_depth = 0;
  euler_rpy.x = 0;
  euler_rpy.y = 0;
  euler_rpy.z = 0;
  alignment_plane = (bool)riptide_msgs::Constants::PLANE_YZ;
  publish_pneumatics = false;

  roll_factor = CMD_ROLL_RATE / rt;
  pitch_factor = CMD_PITCH_RATE / rt;
  yaw_factor = CMD_YAW_RATE / rt;
  depth_factor = CMD_DEPTH_RATE / rt;

  axes_rear_R2 = 0;
  axes_rear_L2 = 0;
  publishedIMUDisable = false;

  PS3Controller::InitMsgs();

  ROS_INFO("PS3 Controller Reset. Press Start to begin.");
  ROS_INFO("PS3: Press Triangle to begin depth command.");
}

void PS3Controller::InitMsgs()
{
  reset_msg.reset_pwm = true;

  euler_rpy.x = 0;
  euler_rpy.y = 0;
  euler_rpy.z = 0;

  PS3Controller::DisableControllers();
}

// Load parameter from namespace
template <typename T>
void PS3Controller::LoadParam(string param, T &var)
{
  try
  {
    if (!nh.getParam(param, var))
    {
      throw 0;
    }
  }
  catch (int e)
  {
    string ns = nh.getNamespace();
    ROS_ERROR("PS3 Controller Namespace: %s", ns.c_str());
    ROS_ERROR("Critical! Param \"%s/%s\" does not exist or is not accessed correctly. Shutting down.", ns.c_str(), param.c_str());
    ros::shutdown();
  }
}

void PS3Controller::DepthCB(const riptide_msgs::Depth::ConstPtr &depth_msg)
{
  current_depth = depth_msg->depth;
}

// Create rotation matrix from IMU orientation
void PS3Controller::ImuCB(const riptide_msgs::Imu::ConstPtr &imu_msg)
{
  euler_rpy = imu_msg->rpy_deg;
}

void PS3Controller::JoyCB(const sensor_msgs::Joy::ConstPtr &joy)
{
  // Disable/Re-enable Depth Controller and Attitude Controller
  if (joy->buttons[BUTTON_REAR_L1])
  {
    enableAttitude = !enableAttitude;
    if (enableAttitude)
      ROS_INFO("PS3: Re-enabling Attitude Controller");
    else
      ROS_INFO("PS3: Disabling Attitude Controller");
  }

  if (joy->buttons[BUTTON_REAR_R1])
  {
    enableDepth = !enableDepth;
    if (enableDepth)
      ROS_INFO("PS3: Re-enabling Depth Controller");
    else
      ROS_INFO("PS3: Disabling Depth Controller");
  }

  // For some reason, R2 and L2 are initialized to 0
  if (!isR2Init && (joy->axes[AXES_REAR_R2] != 0))
    isR2Init = true;
  if (!isL2Init && (joy->axes[AXES_REAR_L2] != 0))
    isL2Init = true;

  if (isR2Init)
  {
    axes_rear_R2 = 0.5 * (1 - joy->axes[AXES_REAR_R2]);
  }
  else
  {
    axes_rear_R2 = 0;
  }

  if (isL2Init)
  {
    axes_rear_L2 = 0.5 * (1 - joy->axes[AXES_REAR_L2]);
  }
  else
  {
    axes_rear_L2 = 0;
  }

  if (!isReset && joy->buttons[BUTTON_SHAPE_X])
  { // Reset Vehicle (The "X" button)
    isReset = true;
    isStarted = false;
    isDepthInit = false;
    PS3Controller::DisableControllers();
    ROS_INFO("PS3 Controller Reset. Press Start to begin.");
    ROS_INFO("PS3: Press Triangle to begin depth command.");
  }
  else if (isReset)
  { // If reset, must wait for Start button to be pressed
    if (joy->buttons[BUTTON_START])
    {
      isStarted = true;
      isReset = false;
      reset_msg.reset_pwm = false;
      reset_pub.publish(reset_msg);
      cmd_euler_rpy.z = (int)euler_rpy.z;
      cmd_depth.depth = current_depth;
    }
  }
  else if (isStarted)
  {
    // Update Roll and Pitch
    if (joy->buttons[BUTTON_SHAPE_CIRCLE])
    { // Set both roll and pitch to 0 [deg]
      cmd_euler_rpy.x = 0;
      cmd_euler_rpy.y = 0;
      delta_attitude.x = 0;
      delta_attitude.y = 0;
      cmd_moment.vector.x = 0; // For when IMU sin't working, do NOT change roll or pitch
      cmd_moment.vector.y = 0; // For when IMU sin't working, do NOT change roll or pitch
    }
    else
    { // Update roll/pitch angles with CROSS
      if (enableAttitude)
      {
        // Roll
        if (joy->buttons[BUTTON_CROSS_RIGHT])
          delta_attitude.x = roll_factor * (1 + (boost - 1) * joy->buttons[BUTTON_SHAPE_SQUARE]); // Right -> inc roll
        else if (joy->buttons[BUTTON_CROSS_LEFT])
          delta_attitude.x = -roll_factor * (1 + (boost - 1) * joy->buttons[BUTTON_SHAPE_SQUARE]); // Left -> dec roll
        else
        {
          delta_attitude.x = 0;
          // ROS_INFO("FALSE FROM ENABLE");
        }

        // Pitch
        if (joy->buttons[BUTTON_CROSS_UP])
          delta_attitude.y = pitch_factor * (1 + (boost - 1) * joy->buttons[BUTTON_SHAPE_SQUARE]); // Up -> inc pitch (Nose points upward)
        else if (joy->buttons[BUTTON_CROSS_DOWN])
          delta_attitude.y = -pitch_factor * (1 + (boost - 1) * joy->buttons[BUTTON_SHAPE_SQUARE]); //Down -> dec pitch (Nose points downward)
        else
          delta_attitude.y = 0;

        // Yaw
        delta_attitude.z = joy->axes[AXES_STICK_LEFT_LR] * yaw_factor * (1 + (boost - 1) * joy->buttons[BUTTON_SHAPE_SQUARE]);
      }
    }

    if (!enableAttitude)
    {
      delta_attitude.x = 0;
      delta_attitude.y = 0;
      delta_attitude.z = 0;
      cmd_euler_rpy.z = (int)euler_rpy.z;

      // XY Moment
      if (abs(joy->axes[AXES_STICK_LEFT_UD]) < 0.5)
      {
        cmd_moment.vector.x = -joy->axes[AXES_STICK_LEFT_LR] * MAX_X_MOMENT;
        cmd_moment.vector.y = 0;
      }
      else
      {
        cmd_moment.vector.y = joy->axes[AXES_STICK_LEFT_UD] * MAX_Y_MOMENT;
        cmd_moment.vector.x = 0;
      }

      // Z Moment - USE CROSS (not the left joystick)
      //cmd_ang_accel.z = -joy->axes[AXES_STICK_LEFT_LR] * MAX_Z_MOMENT; // CW is positive
      if (joy->buttons[BUTTON_CROSS_RIGHT])
        cmd_moment.vector.z = -0.25 * MAX_Z_MOMENT; // CW is positive
      else if (joy->buttons[BUTTON_CROSS_LEFT])
        cmd_moment.vector.z = 0.25 * MAX_Z_MOMENT;
      else
        cmd_moment.vector.z = 0;
    }

    // Update Depth/Z-accel
    if (joy->buttons[BUTTON_SHAPE_TRIANGLE])
    { // Enable depth controller
      isDepthInit = true;
      //reset_msg.reset_depth = false;
      cmd_force_z.data = 0;
    }
    else if (isDepthInit && enableDepth)
    {
      delta_depth = axes_rear_R2 * depth_factor * (1 + (boost - 1) * joy->buttons[BUTTON_SHAPE_SQUARE]); // Up -> dec depth, Down -> inc depth
      delta_depth -= axes_rear_L2 * depth_factor * (1 + (boost - 1) * joy->buttons[BUTTON_SHAPE_SQUARE]);
      //cmd_force_z.data = -joy->axes[AXES_STICK_LEFT_UD] * MAX_Z_FORCE; // For when depth sensor isn't working
    }
    else if (isDepthInit && !enableDepth)
    {
      cmd_force_z.data = axes_rear_R2 * MAX_Z_FORCE;
      cmd_force_z.data -= axes_rear_L2 * MAX_Z_FORCE;
      cmd_depth.depth = current_depth;
    }
    else
    {
      delta_depth = 0;
      cmd_force_z.data = 0;
      cmd_depth.depth = current_depth;
    }

    /*// Code for using R2 and L2
      if (isR2Init && (1 - joy->axes[AXES_REAR_R2] != 0))                     // If pressed at all, inc z-accel
        cmd_force_z.data = 0.5 * (1 - joy->axes[AXES_REAR_R2]) * MAX_Z_FORCE; // Multiplied by 0.5 to scale axes value from 0 to 1
      else if (isL2Init && (1 - joy->axes[AXES_REAR_L2] != 0))                // If pressed at all, dec z-accel
        cmd_force_z.data = 0.5 * (1 - joy->axes[AXES_REAR_L2]) * MAX_Z_FORCE; // Multiplied by 0.5 to scale axes value from 0 to 1*/
  }

  // Update Linear XY Accel
  cmd_force_x.data = joy->axes[AXES_STICK_RIGHT_UD] * MAX_X_FORCE;  // Surge (X) positive forward
  cmd_force_y.data = -joy->axes[AXES_STICK_RIGHT_LR] * MAX_Y_FORCE; // Sway (Y) positive left

  if (joy->buttons[BUTTON_SELECT])
    alignment_plane = !alignment_plane;

  /*if (joy->buttons[BUTTON_REAR_L1])
    {
      pneumatics_cmd.torpedo_port = true;
      publish_pneumatics = true;
      ROS_INFO("port");
    }
    if (joy->buttons[BUTTON_REAR_R1])
    {
      pneumatics_cmd.torpedo_stbd = true;
      publish_pneumatics = true;
      ROS_INFO("stbd");
    }
    if (joy->buttons[BUTTON_PAIRING])
    {
      pneumatics_cmd.markerdropper = true;
      publish_pneumatics = true;
      ROS_INFO("marker");
    }*/
}

// Run when Reset button is pressed
void PS3Controller::DisableControllers()
{
  reset_msg.reset_pwm = true;

  PS3Controller::DisableAttitude();

  delta_attitude.x = 0;
  delta_attitude.y = 0;
  delta_attitude.z = 0;

  cmd_moment.vector.x = 0;
  cmd_moment.vector.y = 0;
  cmd_moment.vector.z = 0;

  PS3Controller::DisableDepth();
  delta_depth = 0;

  cmd_force_x.data = 0;
  cmd_force_y.data = 0;
  cmd_force_z.data = 0;

  reset_pub.publish(reset_msg);

  pneumatics_cmd.torpedo_port = false;
  pneumatics_cmd.torpedo_stbd = false;
  pneumatics_cmd.manipulator = false;
  pneumatics_cmd.markerdropper = false;
  pneumatics_cmd.duration = 250;

  PS3Controller::PublishCommands();
}

void PS3Controller::DisableAttitude()
{
  if (!IS_ATTITUDE_RESET)
  {
    riptide_msgs::AttitudeCommand roll_cmd;
    riptide_msgs::AttitudeCommand pitch_cmd;
    riptide_msgs::AttitudeCommand yaw_cmd;

    roll_cmd.value = 0;
    pitch_cmd.value = 0;
    yaw_cmd.value = 0;

    roll_cmd.mode = roll_cmd.MOMENT;
    pitch_cmd.mode = pitch_cmd.MOMENT;
    yaw_cmd.mode = yaw_cmd.MOMENT;
    

    IS_ATTITUDE_RESET = true;
    roll_pub.publish(roll_cmd);
    pitch_pub.publish(pitch_cmd);
    yaw_pub.publish(yaw_cmd);
  }
}

void PS3Controller::DisableDepth()
{
  if (!IS_DEPTH_RESET)
  {
    cmd_depth.active = false;
    cmd_depth.depth = 0;

    IS_DEPTH_RESET = true;
    depth_pub.publish(cmd_depth);
  }
}

double PS3Controller::Constrain(double current, double max)
{
  if (current > max)
    return max;
  else if (current < -1 * max)
    return -1 * max;
  return current;
}

void PS3Controller::UpdateCommands()
{

  if (enableAttitude)
  {
    IS_ATTITUDE_RESET = false;

    cmd_euler_rpy.x += delta_attitude.x;
    cmd_euler_rpy.y += delta_attitude.y;
    cmd_euler_rpy.z += delta_attitude.z;

    cmd_euler_rpy.x = PS3Controller::Constrain(cmd_euler_rpy.x, MAX_ROLL);
    cmd_euler_rpy.y = PS3Controller::Constrain(cmd_euler_rpy.y, MAX_PITCH);

    if (cmd_euler_rpy.z > 180)
      cmd_euler_rpy.z -= 360;
    if (cmd_euler_rpy.z < -180)
      cmd_euler_rpy.z += 360;
  }
  else
  {
    PS3Controller::DisableAttitude();
  }

  if (enableDepth)
  {
    IS_DEPTH_RESET = false;

    cmd_depth.active = true;
    cmd_depth.depth += delta_depth;
    cmd_depth.depth = PS3Controller::Constrain(cmd_depth.depth, MAX_DEPTH);
    if (cmd_depth.depth < 0)
      cmd_depth.depth = 0;
  }
  else
  {
    PS3Controller::DisableDepth();
  }
  plane_msg.data = (int)alignment_plane;
}

void PS3Controller::PublishCommands()
{
  if (enableAttitude && !IS_ATTITUDE_RESET)
  {
    riptide_msgs::AttitudeCommand roll_cmd;
    riptide_msgs::AttitudeCommand pitch_cmd;
    riptide_msgs::AttitudeCommand yaw_cmd;

    roll_cmd.value = cmd_euler_rpy.x;
    pitch_cmd.value = cmd_euler_rpy.y;
    yaw_cmd.value = cmd_euler_rpy.z;

    roll_cmd.mode = roll_cmd.POSITION;
    pitch_cmd.mode = pitch_cmd.POSITION;
    yaw_cmd.mode = yaw_cmd.POSITION;
    
    roll_pub.publish(roll_cmd);
    pitch_pub.publish(pitch_cmd);
    yaw_pub.publish(yaw_cmd);
  }
  else
  {
    cmd_moment.header.stamp = ros::Time::now();
    moment_pub.publish(cmd_moment);
  }

  x_force_pub.publish(cmd_force_x);
  y_force_pub.publish(cmd_force_y);

  if (enableDepth && !IS_DEPTH_RESET)
    depth_pub.publish(cmd_depth);
  else
    z_force_pub.publish(cmd_force_z);

  plane_pub.publish(plane_msg);

  /*if (publish_pneumatics)
    pneumatics_pub.publish(pneumatics_cmd);
  pneumatics_cmd.torpedo_port = false;
  pneumatics_cmd.torpedo_stbd = false;
  pneumatics_cmd.manipulator = false;
  pneumatics_cmd.markerdropper = false;
  publish_pneumatics = false;*/
}

// This loop function is critical because it allows for different command rates
void PS3Controller::Loop()
{
  ros::Rate rate(rt); // MUST have this rate in here, since all factors are based on it
  while (ros::ok())
  {
    if (isStarted)
    {
      PS3Controller::UpdateCommands();
      PS3Controller::PublishCommands();
    }
    ros::spinOnce();
    rate.sleep();
  }
}