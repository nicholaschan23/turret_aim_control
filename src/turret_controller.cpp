#include "turret_controller.hpp"

TurretController::TurretController() : Node("turret_controller")
{
    robot_init_parameters();
    robot_init_publishers();
    robot_init_timers();

    // if (!turret_simulate_joint_states)
    // {
    //     set_custom_dynamixel_motor_pid_gains();
    // }
}

void TurretController::robot_init_parameters()
{
    timer_hz = this->declare_parameter<int32_t>("timer_hz", 10);

    turret_simulate_joint_states = this->declare_parameter<bool>("turret_simulate_joint_states", true);

    // Turret velocity PID gain constants, currently set to Dynamixel default values
    kp_pos = this->declare_parameter<int32_t>("kp_pos", 800);
    ki_pos = this->declare_parameter<int32_t>("ki_pos", 0);
    kd_pos = this->declare_parameter<int32_t>("kd_pos", 0);
    k1 = this->declare_parameter<int32_t>("k1", 0);
    k2 = this->declare_parameter<int32_t>("k2", 0);
    kp_vel = this->declare_parameter<int32_t>("kp_vel", 100);
    ki_vel = this->declare_parameter<int32_t>("ki_vel", 1920);

    // End-effector velocity PID gain constants
    kp = this->declare_parameter<float>("kp", 5.0);
    ki = this->declare_parameter<float>("ki", 1.0);
    kd = this->declare_parameter<float>("kd", 0.0);

    // Initialize the end-effector PID buffer matrix size and fill with zeroes
    buffer_n = this->declare_parameter<int32_t>("buffer_n", 10);
    buffer.resize(3, buffer_n);
    buffer.setZero();

    // Names
    turret_name = this->declare_parameter<std::string>("turret_name", "");
    payload_name = this->declare_parameter<std::string>("payload_name", "");

    // Links and joints
    base_link = this->declare_parameter<std::string>("base_link", "");
    turret_pan_link = this->declare_parameter<std::string>("turret_pan_link", "");
    turret_tilt_link = this->declare_parameter<std::string>("turret_tilt_link", "");
    payload_aim_link = this->declare_parameter<std::string>("payload_aim_link", "");
    payload_aim_joint = this->declare_parameter<std::string>("payload_aim_joint", "");
    target_link = this->declare_parameter<std::string>("target_link", "");

    // Topics
    turret_joint_states_topic = this->declare_parameter<std::string>("turret_joint_states_topic", turret_name + "/joint_states");
    payload_joint_states_topic = this->declare_parameter<std::string>("payload_joint_states_topic", payload_name + "/joint_states");

    // Transform topic listener
    tf_buffer = std::make_unique<tf2_ros::Buffer>(this->get_clock());
    tf_listener = std::make_shared<tf2_ros::TransformListener>(*tf_buffer);
    tf_buffer->setUsingDedicatedThread(true);

    // Initial turret joint positions
    q << 0, 0, 0.45;
    dq << 0, 0, 0;
}

void TurretController::robot_init_publishers()
{
    if (turret_simulate_joint_states)
    {
        turret_joint_states_publisher = this->create_publisher<sensor_msgs::msg::JointState>(
            turret_joint_states_topic,
            rclcpp::QoS(1));
    }

    payload_joint_states_publisher = this->create_publisher<sensor_msgs::msg::JointState>(
        payload_joint_states_topic,
        rclcpp::QoS(1));

    // Read by Interbotix SDK to control hardware
    joint_group_command_publisher = this->create_publisher<interbotix_xs_msgs::msg::JointGroupCommand>(
        turret_name + "/commands/joint_group",
        10);
}

void TurretController::robot_init_timers()
{
    joint_goal_timer = this->create_wall_timer(
        std::chrono::milliseconds(static_cast<int>((1.0 / timer_hz) * 1000)),
        std::bind(&TurretController::setTurretJointGoal, this));
}

void TurretController::set_custom_dynamixel_motor_pid_gains()
{
    // Create a client for the service
    std::shared_ptr<rclcpp::Node> node = rclcpp::Node::make_shared("set_motor_pid_gains_client");
    auto client = node->create_client<interbotix_xs_msgs::srv::MotorGains>(turret_name + "/set_motor_pid_gains");

    // Prepare the request
    auto request = std::make_shared<interbotix_xs_msgs::srv::MotorGains::Request>();
    request->cmd_type = std::string("group");
    request->name = turret_name;
    request->kp_pos = kp_pos;
    request->ki_pos = ki_pos;
    request->kd_pos = kd_pos;
    request->k1 = k1;
    request->k2 = k2;
    request->kp_vel = kp_vel;
    request->ki_vel = ki_vel;

    while (!client->wait_for_service(std::chrono::seconds(1)))
    {
        if (!rclcpp::ok())
        {
            RCLCPP_ERROR(get_logger(), "Interrupted while waiting for the service. Exiting.");
            return;
        }
        RCLCPP_INFO(get_logger(), "Waiting for service '%s'...", client->get_service_name());
    }

    // Send the request
    client->async_send_request(request);
}

void TurretController::setTurretJointGoal()
{
    // Fetch transforms
    geometry_msgs::msg::TransformStamped t1;
    geometry_msgs::msg::TransformStamped t2;
    geometry_msgs::msg::TransformStamped t3;
    geometry_msgs::msg::TransformStamped td;
    try
    {
        t1 = tf_buffer->lookupTransform(
            base_link,
            turret_pan_link,
            tf2::TimePointZero);
        t2 = tf_buffer->lookupTransform(
            base_link,
            turret_tilt_link,
            tf2::TimePointZero);
        t3 = tf_buffer->lookupTransform(
            base_link,
            payload_aim_link,
            tf2::TimePointZero);
        td = tf_buffer->lookupTransform(
            base_link,
            target_link,
            tf2::TimePointZero);

        // Rotation
        Eigen::Quaternionf Q1(t1.transform.rotation.w, t1.transform.rotation.x, t1.transform.rotation.y, t1.transform.rotation.z);
        Eigen::Quaternionf Q2(t2.transform.rotation.w, t2.transform.rotation.x, t2.transform.rotation.y, t2.transform.rotation.z);
        Eigen::Quaternionf Q3(t3.transform.rotation.w, t3.transform.rotation.x, t3.transform.rotation.y, t3.transform.rotation.z);

        // [3X1] vector
        Z1 = Q1.toRotationMatrix().block<3, 1>(0, 2);
        Y2 = Q2.toRotationMatrix().block<3, 1>(0, 1);
        X3 = Q3.toRotationMatrix().block<3, 1>(0, 0);
        // RCLCPP_INFO_STREAM(get_logger(), "X3: \n" << X3);

        // Translation
        t1_transform << t1.transform.translation.x, t1.transform.translation.y, t1.transform.translation.z;
        t2_transform << t2.transform.translation.x, t2.transform.translation.y, t2.transform.translation.z;
        t3_transform << t3.transform.translation.x, t3.transform.translation.y, t3.transform.translation.z;
        td_transform << td.transform.translation.x, td.transform.translation.y, td.transform.translation.z;

        // Inverse Kinematics variables
        Eigen::MatrixXf Jacobian(6, 3);
        Eigen::MatrixXf JacobianInv;
        Eigen::VectorXf dx(6, 1);

        // [6X3] matrix
        Jacobian << Z1, Y2, Eigen::Vector3f::Zero(3),
            Z1.cross(t3_transform - t1_transform), Y2.cross(t3_transform - t2_transform), X3;
        // RCLCPP_INFO_STREAM(get_logger(), "jacobian: \n" << Jacobian);

        JacobianInv = Jacobian.completeOrthogonalDecomposition().pseudoInverse();
        // dx << Eigen::Vector3f::Zero(3), kp * (td_transform - t3_transform);
        dx << Eigen::Vector3f::Zero(3), getPIDVelocity();
        dq = JacobianInv * dx;

        // Check if dq(2) would make q(2) go negative, don't let it
        if (q.coeff(2) + dq.coeff(2) < 0.05)
        {
            // RCLCPP_INFO(get_logger(), "Set dq to zero? %f + %f = %f", q.coeff(2), dq.coeff(2), q.coeff(2)+dq.coeff(2));
            dq(2) = 0.05 - q.coeff(2);
        }

        q += dq / timer_hz;

        publishTurretJointGoal();
    }
    catch (const tf2::TransformException &ex)
    {
        RCLCPP_INFO(get_logger(), "Couldn't find transforms: %s", ex.what());
    }

    publishSimJointStates();
}

Eigen::Vector3f TurretController::getPIDVelocity()
{
    Eigen::Vector3f error = td_transform - t3_transform;
    // printVector("error", error);

    updateBuffer(buffer, error);

    Eigen::Vector3f error_dt = calculateErrorDt(buffer, timer_hz);
    // printVector("error_dt", error_dt);

    Eigen::Vector3f error_integral = calculateErrorIntegral(buffer, timer_hz);

    return (kp * error) + (kd * error_dt) - (ki * error_integral);
}

void TurretController::updateBuffer(Eigen::Matrix3Xf &buffer, Eigen::Vector3f value)
{
    // Left shift the buffer discarding the oldest value
    buffer.leftCols(buffer.cols() - 1) = buffer.rightCols(buffer.cols() - 1);

    // Assign new value to last column of the buffer
    buffer.col(buffer.cols() - 1) = value;
}

Eigen::Vector3f TurretController::calculateErrorDt(Eigen::Matrix3Xf &buffer, int hz)
{
    // Calculate time step between consecutive buffer elements
    float dt = 1.0 / hz;

    // Calculate the average rate of change over the buffer
    Eigen::Vector3f error_dt = (buffer.col(buffer.cols() - 1) - buffer.col(0)) / (dt * (buffer.cols() - 1));

    return error_dt;
}

Eigen::Vector3f TurretController::calculateErrorIntegral(Eigen::Matrix3Xf &buffer, int hz)
{
    // Calculate time step between consecutive buffer elements
    float dt = 1.0 / hz;

    // Eigen::Vector3f error_integral = buffer.rowwise().sum() * dt;
    Eigen::Vector3f error_integral = buffer.rowwise().sum() * dt / buffer.cols();

    return error_integral;
}

void TurretController::publishTurretJointGoal()
{
    interbotix_xs_msgs::msg::JointGroupCommand joint_group_command_msg;
    joint_group_command_msg.name = turret_name;
    joint_group_command_msg.cmd = {
        dq.coeff(0),
        dq.coeff(1),
    };
    joint_group_command_publisher->publish(joint_group_command_msg);
}

void TurretController::publishSimJointStates()
{
    sensor_msgs::msg::JointState joint_states_msg;
    joint_states_msg.header.stamp = this->get_clock()->now();

    if (turret_simulate_joint_states)
    {
        joint_states_msg.name = {
            "pan",
            "tilt",
        };
        joint_states_msg.position = {
            q.coeff(0),
            q.coeff(1),
        };
        turret_joint_states_publisher->publish(joint_states_msg);
    }

    // payload aim joint
    joint_states_msg.name = {
        payload_aim_joint,
    };
    joint_states_msg.position = {
        q.coeff(2),
    };
    payload_joint_states_publisher->publish(joint_states_msg);
}

void TurretController::printVector(std::string var, Eigen::Vector3f vector)
{
    RCLCPP_INFO(get_logger(), "%s: <%f, %f, %f>", var.c_str(), vector.coeff(0), vector.coeff(1), vector.coeff(2));
}

int main(int argc, char **argv)
{
    rclcpp::init(argc, argv);
    auto node = std::make_shared<TurretController>();
    rclcpp::spin(node);
    rclcpp::shutdown();
    return 0;
}
