#include <ros/ros.h>
#include <ros/package.h>
#include <tf/transform_broadcaster.h>
#include <tf/transform_listener.h>
#include <kdl/frames.hpp>
#include <string>
#include <XnOpenNI.h>
#include <XnCodecIDs.h>
#include <XnCppWrapper.h>
#include <map>
#include <opencv2/video/tracking.hpp>

#define MAX_USERS 15
#define DISTANCE_THRESHOLD 0.8

std::string genericUserCalibrationFileName;

std::string frame_id("camera_depth_frame");
std::string parent_frame_id("map");
int skeleton_to_track = 0;

xn::Context        g_Context;
xn::DepthGenerator g_DepthGenerator;
xn::UserGenerator  g_UserGenerator;

std::map<int, std::pair<ros::Time, ros::Duration> > users_timeouts;

void XN_CALLBACK_TYPE User_NewUser(xn::UserGenerator&, XnUserID, void*);
void XN_CALLBACK_TYPE User_LostUser(xn::UserGenerator&, XnUserID, void*);
void XN_CALLBACK_TYPE User_OutOfScene(xn::UserGenerator&, XnUserID, void*);
void XN_CALLBACK_TYPE User_BackIntoScene(xn::UserGenerator&, XnUserID, void*);

XnUInt32 calibrationData;

void kalmanInitialization();
void predictAndPublish();
void kalmanPrediction();
void kalmanUpdate(tf::Transform);

bool calcUserTransforms(XnUserID const&, tf::Transform&, tf::Transform&, tf::Transform&, tf::Transform&);
void publishUserTransforms(int, tf::Transform, tf::Transform, tf::Transform, tf::Transform, ros::Time);
void publishAllTransforms();

bool checkCenterOfMass(XnUserID const&);

bool updateParent();

void stopTrackingAll(int);
void startTrackingAll();

tf::StampedTransform parentTransform;

//--------------Kalman Filter-----------------

const int stateSize = 6;
const int measSize = 3;
const int contrSize = 0;

cv::KalmanFilter kf1(stateSize, measSize, contrSize);
cv::KalmanFilter kf2(stateSize, measSize, contrSize);

cv::Mat state(stateSize, 1, CV_32F);
cv::Mat meas(measSize, 1, CV_32F);

double ticks = 0;
double dT;
int notFoundCount = 0;
bool detected = false;
bool isTracking = false;
bool lost = false;

ros::Time lastStamp;

std::deque<bool> direction(5, true);

int main(int argc, char **argv)
{
    ros::init(argc, argv, "hostess_skeleton_tracker");
    ros::NodeHandle nh;

    std::string configFilename = ros::package::getPath("hostess_skeleton_tracker") + "/init/openni_tracker.xml";
    genericUserCalibrationFileName = ros::package::getPath("hostess_skeleton_tracker") + "/init/GenericUserCalibration.bin";

    XnStatus nRetVal;

    while(nh.ok())
    {
    	nRetVal = g_Context.InitFromXmlFile(configFilename.c_str());

    	if(nRetVal != XN_STATUS_OK)
    	{
    		ROS_INFO("InitFromXml failed: %s Retrying in 3 seconds...", xnGetStatusString(nRetVal));
    		ros::Duration(3).sleep();
    	}
    	else
    	{
    		break;
    	}
    }

    nRetVal = g_Context.FindExistingNode(XN_NODE_TYPE_DEPTH, g_DepthGenerator);

    if(nRetVal != XN_STATUS_OK)
	{
		ROS_ERROR("Find depth generator failed: %s", xnGetStatusString(nRetVal));
	}

	nRetVal = g_Context.FindExistingNode(XN_NODE_TYPE_USER, g_UserGenerator);

	if (nRetVal != XN_STATUS_OK)
	{
		nRetVal = g_UserGenerator.Create(g_Context);

	    if (nRetVal != XN_STATUS_OK)
	    {
		    ROS_ERROR("NITE is likely missing: Please install NITE >= 1.5.2.21. Check the readme for download information. Error Info: User generator failed: %s", xnGetStatusString(nRetVal));
            return nRetVal;
	    }
	}

	if (!g_UserGenerator.IsCapabilitySupported(XN_CAPABILITY_SKELETON))
	{
		ROS_INFO("Supplied user generator doesn't support skeleton");
		return EXIT_FAILURE;
	}

	g_UserGenerator.GetSkeletonCap().SetSkeletonProfile(XN_SKEL_PROFILE_UPPER);

    XnCallbackHandle hUserCallbacks;
    g_UserGenerator.RegisterUserCallbacks(User_NewUser, User_LostUser, NULL, hUserCallbacks);

	nRetVal = g_Context.StartGeneratingAll();

	if(nRetVal != XN_STATUS_OK)
	{
		ROS_ERROR("StartGenerating failed: %s", xnGetStatusString(nRetVal));
	}

    nh.getParam("camera_frame_id", frame_id);
    nh.getParam("parent_frame_id", parent_frame_id);

	static tf::TransformListener listener;
	static tf::TransformBroadcaster br;

	kalmanInitialization();

	ros::Time lastDetected;

	while(nh.getParam("skeleton_to_track", skeleton_to_track) && nh.ok())
	{
		updateParent();

		double precTick = ticks;

		ticks = (double)cv::getTickCount();

		dT = (ticks - precTick) / cv::getTickFrequency();

		g_Context.WaitAndUpdateAll();

		if(skeleton_to_track == 0)
		{
			//Searching phase. Keep tracking and publishing transforms of all users in the field of view of the sensor. Nothing else to do in this phase.

			publishAllTransforms();
		}
		else
		{
			//Tracking phase. I have my user to track, i need to apply the Kalman filter to its torso.

			if(!isTracking)
			{
				stopTrackingAll(skeleton_to_track);
				isTracking = true;
			}

			ros::Time now = ros::Time::now();

			if(!lost)
			{
				tf::Transform torso_local, torso_global, head_local, head_global;

				if(calcUserTransforms(skeleton_to_track, torso_local, torso_global, head_local, head_global))
				{
					publishUserTransforms(skeleton_to_track, torso_local, torso_global, head_local, head_global, now);

					kalmanUpdate(torso_global);

					lastDetected = now;
				}
				else
				{
					lost = true;
					startTrackingAll();
				}
			}
			else
			{
				XnUInt16 users_count = MAX_USERS;
				XnUserID users[MAX_USERS];

				g_UserGenerator.GetUsers(users, users_count);

				int closer = 0;
				double min = std::numeric_limits<double>::max();

				for(int i = 0; i < users_count; ++i)
				{
					XnUserID user = users[i];

					tf::Transform torso_local, torso_global, head_local, head_global;

					if(calcUserTransforms(user, torso_local, torso_global, head_local, head_global))
					{
						double current = std::sqrt(std::pow(torso_global.getOrigin().getX() - state.at<float>(0), 2) + std::pow(torso_global.getOrigin().getY() - state.at<float>(1), 2));

						if(current < min)
						{
							min = current;
							closer = user;
						}
					}
				}

				if(closer != 0)
				{
					ROS_INFO("%f", min);
				}

				if(closer != 0 && min < DISTANCE_THRESHOLD)
				{
					skeleton_to_track = closer;
					ros::param::set("skeleton_to_track", skeleton_to_track);
					lost = false;
				}
				else
				{
					if((now - lastDetected).sec >= 3)
					{
						lost = false;
						detected = false;
						isTracking = false;
						skeleton_to_track = 0;
						ros::param::set("skeleton_to_track", skeleton_to_track);
					}
				}
			}

			if(detected)
			{
				predictAndPublish();
			}
		}

		ros::Rate(30).sleep();
	}

	g_Context.Shutdown();
	return EXIT_SUCCESS;
}

void XN_CALLBACK_TYPE User_NewUser(xn::UserGenerator& generator, XnUserID nId, void* pCookie)
{
	ROS_INFO("New User %d.", nId);

	if(calibrationData == NULL)
	{
		g_UserGenerator.GetSkeletonCap().LoadCalibrationDataFromFile(nId, genericUserCalibrationFileName.c_str());
		g_UserGenerator.GetSkeletonCap().SaveCalibrationData(nId, calibrationData);
	}
	else
	{
		g_UserGenerator.GetSkeletonCap().LoadCalibrationData(nId, calibrationData);
	}

	int skeleton_to_track;
	ros::param::get("skeleton_to_track", skeleton_to_track);

	if(skeleton_to_track == 0 || lost)
	{
		g_UserGenerator.GetSkeletonCap().StartTracking(nId);
		ROS_INFO("Start tracking user: %d.", nId);
	}
}

void XN_CALLBACK_TYPE User_LostUser(xn::UserGenerator& generator, XnUserID nId, void* pCookie)
{
	ROS_INFO("Lost user %d. Stop tracking.", nId);

	if(g_UserGenerator.GetSkeletonCap().IsTracking(nId))
	{
		g_UserGenerator.GetSkeletonCap().StopTracking(nId);
	}
}

void XN_CALLBACK_TYPE User_BackIntoScene(xn::UserGenerator& generator, XnUserID nId, void* pCookie)
{
	ROS_INFO("User %d back into scene. Restart tracking.", nId);

	g_UserGenerator.GetSkeletonCap().StartTracking(nId);
}

void XN_CALLBACK_TYPE User_OutOfScene(xn::UserGenerator& generator, XnUserID nId, void* pCookie)
{
	ROS_INFO("User %d out of scene. Stop tracking.", nId);

	g_UserGenerator.GetSkeletonCap().StopTracking(nId);
}

//------------------------- Kalman Filter methods --------------------------

void kalmanInitialization()
{
	cv::setIdentity(kf1.transitionMatrix);
	cv::setIdentity(kf1.errorCovPre);

	kf1.measurementMatrix = cv::Mat::zeros(measSize, stateSize, CV_32F);
	kf1.measurementMatrix.at<float>(0) = 1.0f;
	kf1.measurementMatrix.at<float>(7) = 1.0f;
	kf1.measurementMatrix.at<float>(14) = 1.0f;

	cv::setIdentity(kf1.processNoiseCov, cv::Scalar(1e-2));
	kf1.processNoiseCov.at<float>(21) = 1e1;
	kf1.processNoiseCov.at<float>(28) = 1e1;
	kf1.processNoiseCov.at<float>(35) = 1e1;
	cv::setIdentity(kf1.measurementNoiseCov, cv::Scalar(1e-2));
	cv::setIdentity(kf1.errorCovPost, cv::Scalar(1e-1));

	kf1.transitionMatrix.at<float>(2) = 1.0f;
	kf1.transitionMatrix.at<float>(9) = 1.0f;
	kf1.transitionMatrix.at<float>(16) = 1.0f;

	cv::setIdentity(kf2.transitionMatrix);
	cv::setIdentity(kf2.errorCovPre);

	kf2.measurementMatrix = cv::Mat::zeros(measSize, stateSize, CV_32F);
	kf2.measurementMatrix.at<float>(0) = 1.0f;
	kf2.measurementMatrix.at<float>(7) = 1.0f;
	kf2.measurementMatrix.at<float>(14) = 1.0f;

	cv::setIdentity(kf2.processNoiseCov, cv::Scalar(1e-2));
	kf2.processNoiseCov.at<float>(21) = 1e1;
	kf2.processNoiseCov.at<float>(28) = 1e1;
	kf2.processNoiseCov.at<float>(35) = 1e1;
	cv::setIdentity(kf2.measurementNoiseCov, cv::Scalar(1e-2));
	cv::setIdentity(kf2.errorCovPost, cv::Scalar(1e-1));

	kf2.transitionMatrix.at<float>(2) = 1.0f;
	kf2.transitionMatrix.at<float>(9) = 1.0f;
	kf2.transitionMatrix.at<float>(16) = 1.0f;
}

void predictAndPublish()
{
	kalmanPrediction();

	tf::Transform transform;
	transform.setOrigin(tf::Vector3(state.at<float>(0), state.at<float>(1), state.at<float>(2)));
	transform.setRotation(tf::Quaternion(0, 0, 0, 1));

	static tf::TransformBroadcaster br;
	br.sendTransform(tf::StampedTransform(transform, ros::Time::now(), parent_frame_id, "torso_k"));
}

void kalmanPrediction()
{
	kf2.transitionMatrix.at<float>(2) = dT;
	kf2.transitionMatrix.at<float>(9) = dT;
	kf2.transitionMatrix.at<float>(16) = dT;

	state = kf2.predict();
}

void kalmanUpdate(tf::Transform transform)
{
	if(!detected)
	{
		detected = true;

		kf1.statePre.at<float>(0) = transform.getOrigin().getX();
		kf1.statePre.at<float>(1) = transform.getOrigin().getY();
		kf1.statePre.at<float>(2) = transform.getOrigin().getZ();
		kf1.statePre.at<float>(3) = 0;
		kf1.statePre.at<float>(4) = 0;
		kf1.statePre.at<float>(5) = 0;

		kf2.statePre.at<float>(0) = transform.getOrigin().getX();
		kf2.statePre.at<float>(1) = transform.getOrigin().getY();
		kf2.statePre.at<float>(2) = transform.getOrigin().getZ();
		kf2.statePre.at<float>(3) = 0;
		kf2.statePre.at<float>(4) = 0;
		kf2.statePre.at<float>(5) = 0;

		kf1.predict();
		kf2.predict();
	}
	else
	{
		/*direction.pop_front();

		ROS_INFO("Delta Y: %f", fabs(meas.at<float>(1) - transform.getOrigin().getY()));

		if(fabs(meas.at<float>(0) - transform.getOrigin().getX()) > fabs(meas.at<float>(1) - transform.getOrigin().getY()))
		{
			direction.push_back(true);
		}
		else
		{
			direction.push_back(false);
		}

		int counter = 0;

		for(int i = 0; i < direction.size(); ++i)
		{
			if(direction[i] == true)
			{
				counter++;
			}
		}

		if(counter >= 3)
		{
			kf1.processNoiseCov.at<float>(21) = 1e1;
			kf1.processNoiseCov.at<float>(28) = 1;

			kf2.processNoiseCov.at<float>(21) = 1e1;
			kf2.processNoiseCov.at<float>(28) = 1;
		}
		else
		{
			kf1.processNoiseCov.at<float>(21) = 1;
			kf1.processNoiseCov.at<float>(28) = 1e1;

			kf2.processNoiseCov.at<float>(21) = 1;
			kf2.processNoiseCov.at<float>(28) = 1e1;
		}
*/
		meas.at<float>(0) = transform.getOrigin().getX();
		meas.at<float>(1) = transform.getOrigin().getY();
		meas.at<float>(2) = transform.getOrigin().getZ();

		kf1.correct(meas);

		kf1.transitionMatrix.at<float>(2) = dT;
		kf1.transitionMatrix.at<float>(9) = dT;
		kf1.transitionMatrix.at<float>(16) = dT;

		state = kf1.predict();

		cv::Mat meas2(measSize, 1, CV_32F);

		meas2.at<float>(0) = state.at<float>(0);
		meas2.at<float>(1) = state.at<float>(1);
		meas2.at<float>(2) = state.at<float>(2);

		kf2.correct(meas2);
	}
}


//------------------------------------------------------------------------------


void publishUserTransforms(int user, tf::Transform torso_local, tf::Transform torso_global, tf::Transform head_local, tf::Transform head_global, ros::Time now)
{
    static tf::TransformBroadcaster broadcaster;

    std::ostringstream oss;

	oss << "torso_" << user;
	broadcaster.sendTransform(tf::StampedTransform(torso_local, now, frame_id, oss.str()));

	oss.str("");
	oss.clear();
	oss << "global_torso_" << user;
	broadcaster.sendTransform(tf::StampedTransform(torso_global, now, parent_frame_id, oss.str()));

	oss.str("");
	oss.clear();
	oss << "head_" << user;
	broadcaster.sendTransform(tf::StampedTransform(head_local, now, frame_id, oss.str()));

	oss.str("");
	oss.clear();
	oss << "global_head_" << user;
	broadcaster.sendTransform(tf::StampedTransform(head_global, now, parent_frame_id, oss.str()));
}

void publishAllTransforms()
{
    XnUInt16 users_count = MAX_USERS;
    XnUserID users[MAX_USERS];

    g_UserGenerator.GetUsers(users, users_count);

    ros::Time now = ros::Time::now();

    for(int i = 0; i < users_count; ++i)
    {
        XnUserID user = users[i];

        tf::Transform torso_local, torso_global, head_local, head_global;

		if(calcUserTransforms(user, torso_local, torso_global, head_local, head_global))
		{
			publishUserTransforms(user, torso_local, torso_global, head_local, head_global, now);
		}
    }
}

bool checkCenterOfMass(XnUserID const& user)
{
	XnPoint3D center_of_mass;
	XnStatus status = g_UserGenerator.GetCoM(user, center_of_mass);

	if(status != XN_STATUS_OK || center_of_mass.X == 0 || center_of_mass.Y == 0 || center_of_mass.Z == 0)
    {
		return false;
    }
	else
	{
		return true;
	}
}

bool calcUserTransforms(XnUserID const& user, tf::Transform& torso_local, tf::Transform& torso_global, tf::Transform& head_local, tf::Transform& head_global)
{
	XnSkeletonJointPosition torso_position, head_position;
	XnSkeletonJointOrientation torso_orientation, head_orientation;

	if(g_UserGenerator.GetSkeletonCap().IsTracking(user) && checkCenterOfMass(user))
	{
		g_UserGenerator.GetSkeletonCap().GetSkeletonJointPosition(user, XN_SKEL_TORSO, torso_position);
		g_UserGenerator.GetSkeletonCap().GetSkeletonJointOrientation(user, XN_SKEL_TORSO, torso_orientation);
		g_UserGenerator.GetSkeletonCap().GetSkeletonJointPosition(user, XN_SKEL_HEAD, head_position);
		g_UserGenerator.GetSkeletonCap().GetSkeletonJointOrientation(user, XN_SKEL_HEAD, head_orientation);

		if(torso_position.fConfidence < 1 || head_position.fConfidence < 1)
		{
			return false;
		}
	}
	else
	{
		return false;
	}

	double torso_x = -torso_position.position.X / 1000.0;
	double torso_y = torso_position.position.Y / 1000.0;
	double torso_z = torso_position.position.Z / 1000.0;
	double head_x = -head_position.position.X / 1000.0;
	double head_y = head_position.position.Y / 1000.0;
	double head_z = head_position.position.Z / 1000.0;

	XnFloat* torso_m = torso_orientation.orientation.elements;
	KDL::Rotation torso_rotation(torso_m[0], torso_m[1], torso_m[2], torso_m[3], torso_m[4], torso_m[5], torso_m[6], torso_m[7], torso_m[8]);
	double torso_qx, torso_qy, torso_qz, torso_qw;
	torso_rotation.GetQuaternion(torso_qx, torso_qy, torso_qz, torso_qw);
	torso_local.setOrigin(tf::Vector3(torso_x, torso_y, torso_z));
	torso_local.setRotation(tf::Quaternion(torso_qx, -torso_qy, -torso_qz, torso_qw));

	XnFloat* head_m = head_orientation.orientation.elements;
	KDL::Rotation head_rotation(head_m[0], head_m[1], head_m[2], head_m[3], head_m[4], head_m[5], head_m[6], head_m[7], head_m[8]);
	double head_qx, head_qy, head_qz, head_qw;
	head_rotation.GetQuaternion(head_qx, head_qy, head_qz, head_qw);
	head_local.setOrigin(tf::Vector3(head_x, head_y, head_z));
	head_local.setRotation(tf::Quaternion(head_qx, -head_qy, -head_qz, head_qw));

	tf::Transform change_frame;
	change_frame.setOrigin(tf::Vector3(0, 0, 0));
	tf::Quaternion frame_rotation;
	frame_rotation.setEulerZYX(1.5708, 0, 1.5708);
	change_frame.setRotation(frame_rotation);

	torso_local = change_frame * torso_local;
	head_local = change_frame * head_local;

	torso_global = parentTransform * torso_local;
	head_global = parentTransform * head_local;

	return true;
}

bool updateParent()
{
	static tf::TransformListener listener;

	try
	{
		listener.lookupTransform(parent_frame_id, frame_id, ros::Time(0), parentTransform);
		return true;
	}
	catch(tf::TransformException& ex)
	{
		return false;
	}
}

void stopTrackingAll(int current_user)
{
	XnUInt16 users_count = MAX_USERS;
	XnUserID users[MAX_USERS];

	g_UserGenerator.GetUsers(users, users_count);

	for(int i = 0; i < users_count; ++i)
	{
		XnUserID user = users[i];

		if(user != current_user && g_UserGenerator.GetSkeletonCap().IsTracking(user))
		{
			g_UserGenerator.GetSkeletonCap().StopTracking(user);
		}
	}
}

void startTrackingAll()
{
	XnUInt16 users_count = MAX_USERS;
	XnUserID users[MAX_USERS];

	g_UserGenerator.GetUsers(users, users_count);

	for(int i = 0; i < users_count; ++i)
	{
		XnUserID user = users[i];

		g_UserGenerator.GetSkeletonCap().StartTracking(user);
	}
}