#ifndef PAN_CONTROLLER_HPP_
#define PAN_CONTROLLER_HPP_

#include <ros/ros.h>
#include <pluginlib/class_loader.h>
#include <cyton_wrapper/dynamixelIO_wrapper_base.h>

class PanController
{
	private:
		ros::NodeHandle private_nh_;

		boost::shared_ptr <pluginlib::ClassLoader<dynamixelIO_wrapper_base::DynamixelIO_Base> > dxio_loader;

		boost::shared_ptr<dynamixelIO_wrapper_base::DynamixelIO_Base> dxio;

		int mDeviceIndex, mBaudnum, mUpdateRate;
		std::string mYamlPath;

	public:
		PanController(ros::NodeHandle&);
		void sendCommand();
};

#endif /* PAN_CONTROLLER_HPP_ */
