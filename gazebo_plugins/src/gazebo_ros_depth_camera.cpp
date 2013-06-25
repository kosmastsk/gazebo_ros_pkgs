/*
 * Copyright 2013 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/
/*
   Desc: GazeboRosDepthCamera plugin for simulating cameras in Gazebo
   Author: John Hsu
   Date: 24 Sept 2008
 */

#include <algorithm>
#include <assert.h>
#include <boost/thread/thread.hpp>
#include <boost/bind.hpp>

#include <gazebo_plugins/gazebo_ros_depth_camera.h>

#include <gazebo/sensors/Sensor.hh>
#include <gazebo/sdf/interface/SDF.hh>
#include <gazebo/sensors/SensorTypes.hh>

// for creating PointCloud2 from pcl point cloud
#include "pcl/ros/conversions.h"

#include <tf/tf.h>

namespace gazebo
{
// Register this plugin with the simulator
GZ_REGISTER_SENSOR_PLUGIN(GazeboRosDepthCamera)

////////////////////////////////////////////////////////////////////////////////
// Constructor
GazeboRosDepthCamera::GazeboRosDepthCamera()
{
  this->point_cloud_connect_count_ = 0;
  this->depth_info_connect_count_ = 0;
  this->last_depth_image_camera_info_update_time_ = common::Time(0);
  this->advertised_ = false;
}

////////////////////////////////////////////////////////////////////////////////
// Destructor
GazeboRosDepthCamera::~GazeboRosDepthCamera()
{
}

////////////////////////////////////////////////////////////////////////////////
// Load the controller
void GazeboRosDepthCamera::Load(sensors::SensorPtr _parent, sdf::ElementPtr _sdf)
{
  DepthCameraPlugin::Load(_parent, _sdf);

  // copying from DepthCameraPlugin into GazeboRosCameraUtils
  this->parentSensor_ = this->parentSensor;
  this->width_ = this->width;
  this->height_ = this->height;
  this->depth_ = this->depth;
  this->format_ = this->format;
  this->camera_ = this->depthCamera;

  // using a different default
  if (!_sdf->GetElement("imageTopicName"))
    this->image_topic_name_ = "ir/image_raw";
  if (!_sdf->HasElement("cameraInfoTopicName"))
    this->camera_info_topic_name_ = "ir/camera_info";

  // point cloud stuff
  if (!_sdf->GetElement("pointCloudTopicName"))
    this->point_cloud_topic_name_ = "points";
  else
    this->point_cloud_topic_name_ = _sdf->GetElement("pointCloudTopicName")->GetValueString();

  // depth image stuff
  if (!_sdf->GetElement("depthImageTopicName"))
    this->depth_image_topic_name_ = "depth/image_raw";
  else
    this->depth_image_topic_name_ = _sdf->GetElement("depthImageTopicName")->GetValueString();

  if (!_sdf->GetElement("depthImageCameraInfoTopicName"))
    this->depth_image_camera_info_topic_name_ = "depth/camera_info";
  else
    this->depth_image_camera_info_topic_name_ = _sdf->GetElement("depthImageCameraInfoTopicName")->GetValueString();

  if (!_sdf->GetElement("pointCloudCutoff"))
    this->point_cloud_cutoff_ = 0.4;
  else
    this->point_cloud_cutoff_ = _sdf->GetElement("pointCloudCutoff")->GetValueDouble();

  GazeboRosCameraUtils::Load(_parent, _sdf);
}

void GazeboRosDepthCamera::Advertise()
{
  ros::AdvertiseOptions point_cloud_ao =
    ros::AdvertiseOptions::create<sensor_msgs::PointCloud2 >(
      this->point_cloud_topic_name_,1,
      boost::bind( &GazeboRosDepthCamera::PointCloudConnect,this),
      boost::bind( &GazeboRosDepthCamera::PointCloudDisconnect,this),
      ros::VoidPtr(), &this->camera_queue_);
  this->point_cloud_pub_ = this->rosnode_->advertise(point_cloud_ao);

  ros::AdvertiseOptions depth_image_ao =
    ros::AdvertiseOptions::create< sensor_msgs::Image >(
      this->depth_image_topic_name_,1,
      boost::bind( &GazeboRosDepthCamera::DepthImageConnect,this),
      boost::bind( &GazeboRosDepthCamera::DepthImageDisconnect,this),
      ros::VoidPtr(), &this->camera_queue_);
  this->depth_image_pub_ = this->rosnode_->advertise(depth_image_ao);

  ros::AdvertiseOptions depth_image_camera_info_ao =
    ros::AdvertiseOptions::create<sensor_msgs::CameraInfo>(
        this->depth_image_camera_info_topic_name_,1,
        boost::bind( &GazeboRosDepthCamera::DepthInfoConnect,this),
        boost::bind( &GazeboRosDepthCamera::DepthInfoDisconnect,this),
        ros::VoidPtr(), &this->camera_queue_);
  this->depth_image_camera_info_pub_ = this->rosnode_->advertise(depth_image_camera_info_ao);

  this->advertised_ = true;
}


////////////////////////////////////////////////////////////////////////////////
// Increment count
void GazeboRosDepthCamera::PointCloudConnect()
{
  this->point_cloud_connect_count_++;
  this->image_connect_count_++;
  this->parentSensor->SetActive(true);
}
////////////////////////////////////////////////////////////////////////////////
// Decrement count
void GazeboRosDepthCamera::PointCloudDisconnect()
{
  this->point_cloud_connect_count_--;
  this->image_connect_count_--;
  if (this->point_cloud_connect_count_ <= 0)
    this->parentSensor->SetActive(false);
}

////////////////////////////////////////////////////////////////////////////////
// Increment count
void GazeboRosDepthCamera::DepthImageConnect()
{
  this->depth_image_connect_count_++;
  this->parentSensor->SetActive(true);
}
////////////////////////////////////////////////////////////////////////////////
// Decrement count
void GazeboRosDepthCamera::DepthImageDisconnect()
{
  this->depth_image_connect_count_--;
}

////////////////////////////////////////////////////////////////////////////////
// Increment count
void GazeboRosDepthCamera::DepthInfoConnect()
{
  this->depth_info_connect_count_++;
}
////////////////////////////////////////////////////////////////////////////////
// Decrement count
void GazeboRosDepthCamera::DepthInfoDisconnect()
{
  this->depth_info_connect_count_--;
}

////////////////////////////////////////////////////////////////////////////////
// Update the controller
void GazeboRosDepthCamera::OnNewDepthFrame(const float *_image,
    unsigned int _width, unsigned int _height, unsigned int _depth,
    const std::string &_format)
{
  if (!this->initialized_ || this->height_ <=0 || this->width_ <=0)
    return;

  if (!this->advertised_)
    Advertise();

  this->depth_sensor_update_time_ = this->parentSensor->GetLastUpdateTime();
  if (this->parentSensor->IsActive())
  {
    if (this->point_cloud_connect_count_ <= 0 &&
        this->depth_image_connect_count_ <= 0 &&
        this->image_connect_count_ <= 0)
    {
      this->parentSensor->SetActive(false);
    }
    else
    {
      if (this->point_cloud_connect_count_ > 0)
        this->FillPointdCloud(_image);

      if (this->depth_image_connect_count_ > 0)
        this->FillDepthImage(_image);
    }
  }
  else
  {
    if (this->point_cloud_connect_count_ > 0 ||
        this->depth_image_connect_count_ <= 0)
      // do this first so there's chance for sensor to run 1 frame after activate
      this->parentSensor->SetActive(true);
  }
}

///////////////////////////////////////////////////////////////////////////////
// Update the controller
void GazeboRosDepthCamera::OnNewRGBPointCloud(const float *_pcd,
    unsigned int _width, unsigned int _height, unsigned int _depth,
    const std::string &_format)
{
  if (!this->initialized_ || this->height_ <=0 || this->width_ <=0)
    return;

  if (!this->advertised_)
    Advertise();

  this->depth_sensor_update_time_ = this->parentSensor->GetLastUpdateTime();
  if (!this->parentSensor->IsActive())
  {
    if (this->point_cloud_connect_count_ > 0)
      // do this first so there's chance for sensor to run 1 frame after activate
      this->parentSensor->SetActive(true);
  }
  else
  {
    if (this->point_cloud_connect_count_ > 0)
    {
      this->lock_.lock();
      this->point_cloud_msg_.header.frame_id = this->frame_name_;
      this->point_cloud_msg_.header.stamp.sec = this->depth_sensor_update_time_.sec;
      this->point_cloud_msg_.header.stamp.nsec = this->depth_sensor_update_time_.nsec;
      this->point_cloud_msg_.width = this->width;
      this->point_cloud_msg_.height = this->height;
      this->point_cloud_msg_.row_step = this->point_cloud_msg_.point_step * this->width;

      pcl::PointCloud<pcl::PointXYZRGB> point_cloud;
      point_cloud.points.resize(0);
      point_cloud.is_dense = true;

      for (unsigned int i = 0; i < _width; i++)
      {
        for (unsigned int j = 0; j < _height; j++)
        {
          unsigned int index = (j * _width) + i;
          pcl::PointXYZRGB point;
          point.x = _pcd[4 * index];
          point.y = _pcd[4 * index + 1];
          point.z = _pcd[4 * index + 2];
          point.rgb = _pcd[4 * index + 3];
          point_cloud.points.push_back(point);
          if (i == _width /2 && j == _height / 2)
          {
            uint32_t rgb = *reinterpret_cast<int*>(&point.rgb);
            uint8_t r = (rgb >> 16) & 0x0000ff;
            uint8_t g = (rgb >> 8)  & 0x0000ff;
            uint8_t b = (rgb)       & 0x0000ff;
            std::cerr << (int)r << " " << (int)g << " " << (int)b << "\n";
          }
        }
      }
      point_cloud.header = this->point_cloud_msg_.header;
      pcl::toROSMsg(point_cloud, this->point_cloud_msg_);

      this->point_cloud_pub_.publish(this->point_cloud_msg_);
      this->lock_.unlock();
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
// Update the controller
void GazeboRosDepthCamera::OnNewImageFrame(const unsigned char *_image,
    unsigned int _width, unsigned int _height, unsigned int _depth,
    const std::string &_format)
{
  if (!this->initialized_ || this->height_ <=0 || this->width_ <=0)
    return;

  if (!this->advertised_)
    Advertise();

  //ROS_ERROR("camera_ new frame %s %s",this->parentSensor_->GetName().c_str(),this->frame_name_.c_str());
  this->sensor_update_time_ = this->parentSensor->GetLastUpdateTime();

  if (!this->parentSensor->IsActive())
  {
    if (this->image_connect_count_ > 0)
      // do this first so there's chance for sensor to run 1 frame after activate
      this->parentSensor->SetActive(true);
  }
  else
  {
    if (this->image_connect_count_ > 0)
      this->PutCameraData(_image);
  }
}

////////////////////////////////////////////////////////////////////////////////
// Put camera data to the interface
void GazeboRosDepthCamera::FillPointdCloud(const float *_src)
{
  this->lock_.lock();

  this->point_cloud_msg_.header.frame_id = this->frame_name_;
  this->point_cloud_msg_.header.stamp.sec = this->depth_sensor_update_time_.sec;
  this->point_cloud_msg_.header.stamp.nsec = this->depth_sensor_update_time_.nsec;
  this->point_cloud_msg_.width = this->width;
  this->point_cloud_msg_.height = this->height;
  this->point_cloud_msg_.row_step = this->point_cloud_msg_.point_step * this->width;

  ///copy from depth to point cloud message
  FillPointCloudHelper(this->point_cloud_msg_,
                 this->height,
                 this->width,
                 this->skip_,
                 (void*)_src );

  this->point_cloud_pub_.publish(this->point_cloud_msg_);

  this->lock_.unlock();
}

////////////////////////////////////////////////////////////////////////////////
// Put depth image data to the interface
void GazeboRosDepthCamera::FillDepthImage(const float *_src)
{
  this->lock_.lock();
  // copy data into image
  this->depth_image_msg_.header.frame_id = this->frame_name_;
  this->depth_image_msg_.header.stamp.sec = this->depth_sensor_update_time_.sec;
  this->depth_image_msg_.header.stamp.nsec = this->depth_sensor_update_time_.nsec;

  ///copy from depth to depth image message
  FillDepthImageHelper(this->depth_image_msg_,
                 this->height,
                 this->width,
                 this->skip_,
                 (void*)_src );

  this->depth_image_pub_.publish(this->depth_image_msg_);

  this->lock_.unlock();
}


// Fill depth information
bool GazeboRosDepthCamera::FillPointCloudHelper(
    sensor_msgs::PointCloud2 &point_cloud_msg,
    uint32_t rows_arg, uint32_t cols_arg,
    uint32_t step_arg, void* data_arg)
{
  pcl::PointCloud<pcl::PointXYZRGB> point_cloud;

  point_cloud.points.resize(0);
  point_cloud.is_dense = true;

  float* toCopyFrom = (float*)data_arg;
  int index = 0;

  double hfov = this->parentSensor->GetDepthCamera()->GetHFOV().Radian();
  double fl = ((double)this->width) / (2.0 *tan(hfov/2.0));

  // convert depth to point cloud
  for (uint32_t j=0; j<rows_arg; j++)
  {
    double pAngle;
    if (rows_arg>1) pAngle = atan2( (double)j - 0.5*(double)(rows_arg-1), fl);
    else            pAngle = 0.0;

    for (uint32_t i=0; i<cols_arg; i++)
    {
      double yAngle;
      if (cols_arg>1) yAngle = atan2( (double)i - 0.5*(double)(cols_arg-1), fl);
      else            yAngle = 0.0;

      double depth = toCopyFrom[index++];

      // in optical frame
      // hardcoded rotation rpy(-M_PI/2, 0, -M_PI/2) is built-in
      // to urdf, where the *_optical_frame should have above relative
      // rotation from the physical camera *_frame
      pcl::PointXYZRGB point;
      point.x      = depth * tan(yAngle);
      point.y      = depth * tan(pAngle);
      if(depth > this->point_cloud_cutoff_)
      {
        point.z    = depth;
      }
      else //point in the unseeable range
      {
        point.x = point.y = point.z = std::numeric_limits<float>::quiet_NaN ();
        point_cloud.is_dense = false;
      }

      // put image color data for each point
      uint8_t*  image_src = (uint8_t*)(&(this->image_msg_.data[0]));
      if (this->image_msg_.data.size() == rows_arg*cols_arg*3)
      {
        // color
        point.r = image_src[i*3+j*cols_arg*3+0];
        point.g = image_src[i*3+j*cols_arg*3+1];
        point.b = image_src[i*3+j*cols_arg*3+2];
      }
      else if (this->image_msg_.data.size() == rows_arg*cols_arg)
      {
        // mono (or bayer?  @todo; fix for bayer)
        point.r = image_src[i+j*cols_arg];
        point.g = image_src[i+j*cols_arg];
        point.b = image_src[i+j*cols_arg];
      }
      else
      {
        // no image
        point.r = 0;
        point.g = 0;
        point.b = 0;
      }

      point_cloud.points.push_back(point);
    }
  }

  point_cloud.header = point_cloud_msg.header;
  pcl::toROSMsg(point_cloud, point_cloud_msg);
  return true;
}

// Fill depth information
bool GazeboRosDepthCamera::FillDepthImageHelper(
    sensor_msgs::Image& image_msg,
    uint32_t rows_arg, uint32_t cols_arg,
    uint32_t step_arg, void* data_arg)
{
  image_msg.encoding = sensor_msgs::image_encodings::TYPE_32FC1;
  image_msg.height = rows_arg;
  image_msg.width = cols_arg;
  image_msg.step = sizeof(float) * cols_arg;
  image_msg.data.resize(rows_arg * cols_arg * sizeof(float));
  image_msg.is_bigendian = 0;

  const float bad_point = std::numeric_limits<float>::quiet_NaN();

  float* dest = (float*)(&(image_msg.data[0]));
  float* toCopyFrom = (float*)data_arg;
  int index = 0;

  // convert depth to point cloud
  for (uint32_t j = 0; j < rows_arg; j++)
  {
    for (uint32_t i = 0; i < cols_arg; i++)
    {
      float depth = toCopyFrom[index++];

      if (depth > this->point_cloud_cutoff_)
      {
        dest[i + j * cols_arg] = depth;
      }
      else //point in the unseeable range
      {
        dest[i + j * cols_arg] = bad_point;
      }
    }
  }
  return true;
}

void GazeboRosDepthCamera::PublishCameraInfo()
{
  ROS_DEBUG("publishing default camera info, then depth camera info");
  GazeboRosCameraUtils::PublishCameraInfo();

  if (this->depth_info_connect_count_ > 0)
  {
    this->sensor_update_time_ = this->parentSensor_->GetLastUpdateTime();
    common::Time cur_time = this->world_->GetSimTime();
    if (cur_time - this->last_depth_image_camera_info_update_time_ >= this->update_period_)
    {
      this->PublishCameraInfo(this->depth_image_camera_info_pub_);
      this->last_depth_image_camera_info_update_time_ = cur_time;
    }
  }
}

//@todo: publish disparity similar to openni_camera_deprecated/src/nodelets/openni_nodelet.cpp.
/*
#include <stereo_msgs/DisparityImage.h>
pub_disparity_ = comm_nh.advertise<stereo_msgs::DisparityImage > ("depth/disparity", 5, subscriberChanged2, subscriberChanged2);

void GazeboRosDepthCamera::PublishDisparityImage(const DepthImage& depth, ros::Time time)
{
  stereo_msgs::DisparityImagePtr disp_msg = boost::make_shared<stereo_msgs::DisparityImage > ();
  disp_msg->header.stamp                  = time;
  disp_msg->header.frame_id               = device_->isDepthRegistered () ? rgb_frame_id_ : depth_frame_id_;
  disp_msg->image.header                  = disp_msg->header;
  disp_msg->image.encoding                = sensor_msgs::image_encodings::TYPE_32FC1;
  disp_msg->image.height                  = depth_height_;
  disp_msg->image.width                   = depth_width_;
  disp_msg->image.step                    = disp_msg->image.width * sizeof (float);
  disp_msg->image.data.resize (disp_msg->image.height * disp_msg->image.step);
  disp_msg->T = depth.getBaseline ();
  disp_msg->f = depth.getFocalLength () * depth_width_ / depth.getWidth ();

  /// @todo Compute these values from DepthGenerator::GetDeviceMaxDepth() and the like
  disp_msg->min_disparity = 0.0;
  disp_msg->max_disparity = disp_msg->T * disp_msg->f / 0.3;
  disp_msg->delta_d = 0.125;

  depth.fillDisparityImage (depth_width_, depth_height_, reinterpret_cast<float*>(&disp_msg->image.data[0]), disp_msg->image.step);

  pub_disparity_.publish (disp_msg);
}
*/


}
