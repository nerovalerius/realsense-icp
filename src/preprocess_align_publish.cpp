/*    ___         _            _      __   __  _        _                  ____  ___  
 *   / __|  ___  | |__   ___  | |_    \ \ / / (_)  ___ (_)  ___   _ _     |__ / |   \ 
 *  | (__  / _ \ | '_ \ / _ \ |  _|    \ V /  | | (_-< | | / _ \ | ' \     |_ \ | |) |
 *   \___| \___/ |_.__/ \___/  \__|     \_/   |_| /__/ |_| \___/ |_||_|   |___/ |___/ 
 *                                                                                                                       
 *  Armin Niedermüller
 *
 */

#include <pcl/io/pcd_io.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/io/pcd_io.h>
#include <pcl/pcl_config.h>

#include <pcl/filters/filter.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/filters/passthrough.h>
#include <pcl/filters/statistical_outlier_removal.h>

#include <pcl/features/fpfh.h>
#include <pcl/features/normal_3d.h>

#include <pcl/kdtree/kdtree_flann.h>

#include <pcl/surface/mls.h>
#include <pcl/surface/impl/mls.hpp>

#include <pcl/registration/icp.h>
#include <pcl/registration/icp_nl.h>
#include <pcl/registration/transforms.h>
#include <pcl/registration/ia_ransac.h>

#include <iostream>
#include <ros/ros.h>
#include <ros/topic.h>
#include <ros/console.h>
#include <tf2/LinearMath/Matrix3x3.h>
#include <tf2/LinearMath/Transform.h>
#include <tf2_ros/static_transform_broadcaster.h>
#include <geometry_msgs/TransformStamped.h>
#include <tf2/LinearMath/Quaternion.h> 

#include <sensor_msgs/PointCloud2.h>

// Nonlinear ICP also uses curvature - crete own Point class with x, y, z, curvature
class PointCurvature : public pcl::PointRepresentation<pcl::PointNormal>
{
  using pcl::PointRepresentation<pcl::PointNormal>::nr_dimensions_;

public:
  PointCurvature()
  {
    // x, y, z, curvature
    nr_dimensions_ = 4;
  }

  // Override method
  virtual void copyToFloatArray(const pcl::PointNormal &p, float *out) const
  {
    // < x, y, z, curvature >
    out[0] = p.x;
    out[1] = p.y;
    out[2] = p.z;
    out[3] = p.curvature;
  }
};

// ------------------------------------------------------------------------------------------------------------
//                                                  THE MAIN
// ------------------------------------------------------------------------------------------------------------

int main(int argc, char **argv)
{

  // ------------------------------------------------------
  //  VARIABLES & CLASSES
  // ------------------------------------------------------

  // Step counter
  uint step = 1;

  // Steps switch
  bool passthrough_active = false;
  bool downsampling_active = false;
  bool outlier_active = false;
  bool mls_active = false;
  bool manualalignment_active = false;
  bool publishtoros_active = false;
  bool fpfhalignment_active = false;

  // Transformation Matrix for different steps of alignment
  Eigen::Matrix4f transform = Eigen::Matrix4f::Identity();

  // Argument-Vector
  std::vector<std::string> args;

  // Pointcloud-Vector
  std::vector<pcl::PointCloud<pcl::PointNormal>::Ptr> clouds;

  // ------------------------------------------------------
  //  START ROS NODE
  // ------------------------------------------------------

  // ROS NODE
  ros::init(argc, argv, "preprocess_align_publish");
  std::cout << "PREPROCESS_ALIGN_PUBLISH" << std::endl;



  // ------------------------------------------------------
  //  PROCESS ARGUMENTS
  // ------------------------------------------------------

  if (argc < 2)
  {
    std::cout << "not enough arguments - type --help for more info" << std::endl;
    return -1;
  }

  // Parse Arguments
  for (int i = 1; i < argc; i++)
  {
    args.push_back(std::string(argv[i]));
  }


  // Iterate through arguments
  for (const auto &arg : args) {
    // User needs HELP
    if (arg.find("help") != std::string::npos) {
      std::cout << "PREPROCESS - ALIGN - PUBLISH"
                << "\nPCL-Version: " << PCL_VERSION 
                << "\nThis program is designed to:"
                << "\n 1a. Read two pointclouds from ros PointCloud2 streams"
                << "\n 1b. OR read two pointclouds from .pcd files"
                << "\n 2. Downsample and filter both pointclouds"
                << "\n 3. Smooth Surfaces and make a coarse alignment"
                << "\n 4. Apply an iterative closest point algorithm"
                << "\n 5. Publish the Transformation Matrix to the ros /tf topic"
                << "\n ---------------------------------------------------------"
                << "\n Arguments: <cam_1_pointcloud2_topic> <cam_2_pointcloud2_topic>"
                << "\n OR: <cam_1_pointcloud_file.pcd> <cam_2_pointcloud_file.pcd>"
                << "\n Usually: /cam_1/depth/color/points and /cam_2/depth/color/points"
                << "\n This program only reads the pointclouds and applies an ICP if you give no arguments but the topics or files"
                << "\n ---------------------------------------------------------"
                << "\n The following arguments activate the single steps:"
                << "\n   * allstepsfpfh=true OR allstepsmanual=true\t - activates all steps with manual or fpfh feature pre alignment"
                << "\n   * passthrough=true\t\t\t\t - activates passthrough filter"
                << "\n   * downsampling=true\t\t\t\t - activates downsampling"
                << "\n   * outlier=true\t\t\t\t - activates outlier filter"
                << "\n   * mls=true\t\t\t\t\t - activates mls_smoothing"
                << "\n   * manualalignment=true OR fpfhalignment=true\t - activates manual or fpfh feature pre alignment"
                << "\n   * publishtoros=true\t\t\t\t - activates quaternion transformation publishing to ros /tf topic"
                << std::endl;
      return 0;

    // Arguments are point cloud streams? usually contain "points"  and "/cam" in their path
    } else if (arg.find("points") != std::string::npos && arg.find("/cam") != std::string::npos){      

      // ------------------------------------------------------
      //  STEP 0a - READ POINTCLOUD FROM ROS TOPIC
      // ------------------------------------------------------

      // Output
      std::cout << "STEP " << step << " - READ A POINTCLOUD FROM ROS TOPIC" << std::endl;
      ++step;

      // Define a pointer to a cloud
      pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_ptr(new pcl::PointCloud<pcl::PointXYZ>);
      pcl::PointCloud<pcl::PointNormal>::Ptr cloud_ptr_normal(new pcl::PointCloud<pcl::PointNormal>);

      // An object to catch the next pointcloud from a ROS PointCloud2 Topic
      sensor_msgs::PointCloud2ConstPtr cloud_msg = ros::topic::waitForMessage<sensor_msgs::PointCloud2>(arg, ros::Duration(10));
      // Convert the catched message into a pointcloud object
      pcl::fromROSMsg(*cloud_msg, *cloud_ptr);
      
      // Copy PointXYZ to PointNormal
      pcl::copyPointCloud(*cloud_ptr, *cloud_ptr_normal);

      // Add the point cloud to the list of point clouds
      clouds.push_back(cloud_ptr_normal);

    // Arguments are point cloud streams? usually contain "points" in their path
    } else if (arg.find(".pcd") != std::string::npos) { 

      // ------------------------------------------------------
      //  STEP 0b - READ POINTCLOUD FROM FILE
      // ------------------------------------------------------
      
      // Output
      std::cout << "STEP " << step << " - READ A POINTCLOUD FROM PCD FILE" << std::endl;
      ++step;

      // Define a pointer to a cloud
      pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_ptr(new pcl::PointCloud<pcl::PointXYZ>);
      pcl::PointCloud<pcl::PointNormal>::Ptr cloud_ptr_normal(new pcl::PointCloud<pcl::PointNormal>);

      // Read Cloud from File
      if (pcl::io::loadPCDFile<pcl::PointXYZ> (arg, *cloud_ptr) == -1){ 
        std::cout << "could not read " << arg << std::endl;
        return (-1);
      }

      // Copy PointXYZ to PointNormal
      pcl::copyPointCloud(*cloud_ptr, *cloud_ptr_normal);

      // Add the point cloud to the list of point clouds
      clouds.push_back(cloud_ptr_normal);

    // Activate passthrough filter
    } else if (arg.find("passthrough=true") != std::string::npos) {      
        passthrough_active = true;

    // Activate downsampling
    } else if (arg.find("downsampling=true") != std::string::npos) {      
        downsampling_active = true;

    // Activate outlier filtering
    } else if (arg.find("outlier=true") != std::string::npos) {      
        outlier_active = true;

    // Activate smoothing
    } else if (arg.find("mls=true") != std::string::npos) {      
        mls_active = true;

    // Activate manual coarse alignment
    } else if (arg.find("manualalignment=true") != std::string::npos) {      
        manualalignment_active = true;

    // Activate publishing to ros tf topics
    } else if (arg.find("publishtoros=true") != std::string::npos) {      
        publishtoros_active = true;

    // Activate automatic fpfh feature alignment
    } else if (arg.find("fpfhalignment=true") != std::string::npos) {      
        fpfhalignment_active = true;

    // Activate all steps with manual alignment
    } else if (arg.find("allstepsmanual=true") != std::string::npos) {       
        passthrough_active = true;           
        downsampling_active = true;  
        outlier_active = true; 
        mls_active = true;   
        manualalignment_active = true;   
        publishtoros_active = true;     

    // Activate all steps with fpfh feature alignment
    } else if (arg.find("allstepsfpfh=true") != std::string::npos) {       
        passthrough_active = true;           
        downsampling_active = true;  
        outlier_active = true; 
        mls_active = true;   
        fpfhalignment_active = true; 
        publishtoros_active = true;     

    // No proper arguments given
    } else {
      std::cout << "No proper arguments given - type --help for more info " << std::endl;
      return -1;
    }

  }

  // Got two Pointclouds?
  if (clouds.size() != 2){
      std::cout << "No proper pointclouds given" << std::endl;
    return -1;
  }


  // ------------------------------------------------------
  //  PASSTHROUGH_FILTER
  // ------------------------------------------------------


  // Active?
  if (passthrough_active){

    // Output
    std::cout << "STEP " << step << " - PASSTHROUGH_FILTER" << std::endl;
    ++step;

    // Configure the filter
    pcl::PassThrough<pcl::PointNormal> passthrough_filter;
    passthrough_filter.setFilterFieldName("z");

    // Set filter from 0.5m to 2.1m
    passthrough_filter.setFilterLimits(0.5, 2.1);

    // Apply passthrough filter
    for (auto &cloud : clouds)
    {
      passthrough_filter.setInputCloud(cloud);
      passthrough_filter.filter(*cloud);
    }
  }


  // ------------------------------------------------------
  //  DOWNSAMPLING
  // ------------------------------------------------------


  // Active?
  if (downsampling_active){
    // Output
    std::cout << "STEP " << step << " - DOWNSAMPLING" << std::endl;
    ++step;

    // Downsampling of the point clouds using a voxelgrid filter
    // significantly reduces computing time in the next steps

    // Configure the filter
    pcl::VoxelGrid<pcl::PointNormal> voxelgrid_filter;
    voxelgrid_filter.setLeafSize(0.015, 0.015, 0.015);

    // Apply voxel grid filter
    for (auto &cloud : clouds)
    {
      voxelgrid_filter.setInputCloud(cloud);
      voxelgrid_filter.filter(*cloud);
    }
  }


  // ------------------------------------------------------
  //  REMOVE OUTLIERS
  // ------------------------------------------------------

  // Active ?
  if (outlier_active){
    // Output
    std::cout << "STEP " << step << " - REMOVE OUTLIERS" << std::endl;
    ++step;

    // Configure the filter
    pcl::StatisticalOutlierRemoval<pcl::PointNormal> outlier_filter;

    // Apply outlier filter
    for (auto &cloud : clouds)
    {
      outlier_filter.setInputCloud(cloud);
      outlier_filter.setMeanK(50);
      outlier_filter.setStddevMulThresh(1.0);
      outlier_filter.filter(*cloud);
    }
  }


  // ------------------------------------------------------
  //  SMOOTHE SURFACES - MLS
  // ------------------------------------------------------


  // Active ?
  if (mls_active){
    // Output
    std::cout << "STEP " << step << " - SMOOTHE SURFACES" << std::endl;
    ++step;

    // KD-Tree as search method for moving least squares algorithm
    pcl::search::KdTree<pcl::PointNormal>::Ptr kd_tree(new pcl::search::KdTree<pcl::PointNormal>);

    // Configure the filter
    pcl::MovingLeastSquares<pcl::PointNormal, pcl::PointNormal> mls_smoothing;
    mls_smoothing.setComputeNormals(true);
    mls_smoothing.setPolynomialOrder(2);
    mls_smoothing.setSearchMethod(kd_tree);
    mls_smoothing.setSearchRadius(0.03);

    // Apply moving least squares algorithm
    for (auto &cloud : clouds) {

      mls_smoothing.setInputCloud(cloud);

      // MLS needs a second cloud to work
      pcl::PointCloud<pcl::PointNormal> smoothed_cloud;
      mls_smoothing.process(smoothed_cloud);
      *cloud = smoothed_cloud;

      std::vector<int> indices;
      
      // Otherwise ICP would throw an Inf / NaN Error
      if (!cloud->is_dense){
        pcl::removeNaNFromPointCloud(*cloud, *cloud, indices);
      }

      // Otherwise ICP would throw an Inf / NaN Error
      pcl::removeNaNNormalsFromPointCloud(*cloud, *cloud, indices);

    }
  }


  // ------------------------------------------------------
  //  COMPUTE FPFH FEATURES AND ALIGN VIA SAMPLE CONSENSUS INITIAL ALIGNMENT
  // ------------------------------------------------------

  // ----------------------
  //  COMPUTE FPFH FEATURES
  // ----------------------

  // Active ?
  if (fpfhalignment_active == true && manualalignment_active == false){
    // Output
    std::cout << "STEP " << step << " - COMPUTE FPFH FEATURES" << std::endl;
    ++step;

    // Pointcloud-Vector
    std::vector<pcl::PointCloud<pcl::FPFHSignature33>::Ptr> fpfh_features;

    // Create the FPFH estimation class,
    pcl::FPFHEstimation<pcl::PointNormal, pcl::PointNormal, pcl::FPFHSignature33> fpfh;

    // Search tree
    pcl::search::KdTree<pcl::PointNormal>::Ptr tree (new pcl::search::KdTree<pcl::PointNormal>);

    // Define tree as search method
    fpfh.setSearchMethod (tree);

    // Use all neighbors inside 5cm radius
    fpfh.setRadiusSearch (0.05);

    // Compute FPFH Features
    for (auto &cloud : clouds){

      // Set cloud and its normals
      fpfh.setInputCloud (cloud);
      fpfh.setInputNormals(cloud);

      // Output datasets
      pcl::PointCloud<pcl::FPFHSignature33>::Ptr fpfh_feature (new pcl::PointCloud<pcl::FPFHSignature33> ());

      // Compute the features
      fpfh.compute (*fpfh_feature);

      fpfh_features.push_back(fpfh_feature);

    }


    // ----------------------
    //  SAMPLE CONSENSUS INITIAL ALIGNMENT
    // ----------------------

    // Output
    std::cout << "STEP " << step << " - SAMPLE CONSENSUS INITIAL ALIGNMENT" << std::endl;
    ++step;

    // The aligned point cloud
    pcl::PointCloud<pcl::PointNormal> sc_aligned_cloud;

    // Sample Consensus alignment method with fpfh features
    pcl::SampleConsensusInitialAlignment< pcl::PointNormal, pcl::PointNormal, pcl::FPFHSignature33> sc_alignment; 

    // Set input and output clouds and features
    sc_alignment.setInputSource(clouds.at(0));
    sc_alignment.setSourceFeatures (fpfh_features.at(0));

    sc_alignment.setInputTarget (clouds.at(1));
    sc_alignment.setTargetFeatures (fpfh_features.at(1));

    // Align via fpfh features
    sc_alignment.align(sc_aligned_cloud);
  
    // Get transformation matrix
    transform = sc_alignment.getFinalTransformation ();

  }


  // ------------------------------------------------------
  //  COARSE MANUAL ALIGNMENT
  // ------------------------------------------------------

  // Rotation Matrices
  Eigen::Matrix4f transform_x = Eigen::Matrix4f::Identity();
  Eigen::Matrix4f transform_z = Eigen::Matrix4f::Identity();

  // Active?
  if (fpfhalignment_active == false && manualalignment_active == true){
    // Output
    std::cout << "STEP " << step << " - COARSE MANUAL ALIGNMENT" << std::endl;
    ++step;

    // 45 Degrees
    int angle = -45;

    // Rotate fist pointcloud by 45° in X axis
    transform_x(0, 0) = 1;
    transform_x(1, 1) = cos(angle * M_PI / 180);
    transform_x(1, 2) = -sin(angle * M_PI / 180);
    transform_x(2, 1) = sin(angle * M_PI / 180);
    transform_x(2, 2) = cos(angle * M_PI / 180);
    transform_x(3, 3) = 1;

    // 180 Degrees
    angle = 180;

    // Rotate fist pointcloud by 180 in Z axis
    transform_z(0, 0) = cos(angle * M_PI / 180);
    transform_z(0, 1) = -sin(angle * M_PI / 180);
    transform_z(1, 0) = sin(angle * M_PI / 180);
    transform_z(1, 1) = cos(angle * M_PI / 180);
    transform_z(2, 2) = 1;
    transform_z(3, 3) = 1;


    // complete transformation
    transform = transform_z * transform_x;

    // Apply 180 rotation - Z axis
    pcl::transformPointCloud(*clouds.at(0), *clouds.at(0), transform);


  }
  
  
  // ------------------------------------------------------
  //  ITERATIVE CLOSEST POINT ALGORITHM
  // ------------------------------------------------------

  // Output
  std::cout << "STEP " << step << " - ITERATIVE CLOSEST POINT ALGORITHM\n" << std::endl;
  ++step;

  //Points with x, y, z, curvature
  PointCurvature point_xyzc;
  // Curvature has to be weighted in x, y, z
  float weight[4] = {1.0, 1.0, 1.0, 1.0};
  point_xyzc.setRescaleValues(weight);

  // Configure the ICP algorithm
  pcl::IterativeClosestPointNonLinear<pcl::PointNormal, pcl::PointNormal> icp_nonlinear;

  // Maximum distance between two point correspondences - 0.000001 = 10cm
  icp_nonlinear.setTransformationEpsilon(1e-6);
  icp_nonlinear.setMaxCorrespondenceDistance(0.3);

  // Set PointXYZC as representation inside the registration method
  icp_nonlinear.setPointRepresentation(boost::make_shared<const PointCurvature>(point_xyzc));


  // Set both pointclouds - in order to align them
  icp_nonlinear.setInputSource(clouds.at(0));
  icp_nonlinear.setInputTarget(clouds.at(1));

  // Final Transformation
  Eigen::Matrix4f Ti = Eigen::Matrix4f::Identity();
  Eigen::Matrix4f prev;

  // New Pointcloud for transformed source cloud
  auto source_transformed(clouds.at(0));

  // set max iterations per loop
  icp_nonlinear.setMaximumIterations(2);
  
  int icp_loop_count;
  double fitness_score_limit = 0.006;

  std::cout << "Fitness Score Limit: " << fitness_score_limit << std::endl;

  // Iterate two times each loop 
  for (icp_loop_count = 0; icp_loop_count < 30; ++icp_loop_count)
  {

    // Estimate the next transformation Ti (only 2 iterations)
    icp_nonlinear.setInputSource(clouds.at(0));
    icp_nonlinear.align(*source_transformed);

    // Accumulate transformation between each Iteration
    Ti = icp_nonlinear.getFinalTransformation() * Ti;

    // reduce distance between correspondending points each time, the resulting transformation Ti is smaller then threshold
    if (std::abs((icp_nonlinear.getLastIncrementalTransformation() - prev).sum()) < icp_nonlinear.getTransformationEpsilon())
    {
      icp_nonlinear.setMaxCorrespondenceDistance(icp_nonlinear.getMaxCorrespondenceDistance() - 0.001);
    }

    // T(i-1) 
    prev = icp_nonlinear.getLastIncrementalTransformation();

    if((icp_loop_count * icp_nonlinear.getMaximumIterations()) % 10 == 0){
        // Fitness Score
        std::cout << "Iteration: " << (icp_loop_count * icp_nonlinear.getMaximumIterations())
                  << " - Fitness Score: " << icp_nonlinear.getFitnessScore() << std::endl;
    }

    if (icp_nonlinear.getFitnessScore() < fitness_score_limit){
      break;
    }

  }

  std::cout << "\nICP Iterations: " << icp_nonlinear.getMaximumIterations() * icp_loop_count << std::endl;

  // Final Transformation
  std::cout << "\nFinal Transformation:\n" << Ti << "\n" << std::endl;


  // ------------------------------------------------------
  //  PUBLISH TRANSFORMATION MATRIX TO TF TOPICS
  // ------------------------------------------------------

  // Active?
  if (publishtoros_active){
    // Output
    std::cout << "STEP " << step <<" - PUBLISH TRANSFORMATION MATRIX TO ROS TF TOPICS\n" << std::endl;
    ++step;

    // Dont forget the coarse pre-alignment from above! - Only if coarse pre-alignment was performed, otherwise transform_z and _x should be identity matrix
    Eigen::Matrix4f T = Ti * transform;

    // Concatenate both point clouds
    *clouds.at(0) += *clouds.at(1);

    // Write aligned cloud to file
    pcl::io::savePCDFileASCII("./temporal_test_cloud.pcd", *clouds.at(0));

    
    // Rotation Matrix
    tf2::Matrix3x3 rotation_matrix;
    rotation_matrix.setValue(T(0,0), T(0,1), T(0,2),
                      T(1,0), T(1,1), T(1,2),
                      T(2,0), T(2,1), T(2,2)
    );

    // Calculate Quaternions
    tf2::Quaternion quaternion;
    rotation_matrix.getRotation(quaternion);
  
    // Broadcast to tf_static topic
    static tf2_ros::StaticTransformBroadcaster static_broadcaster;
    geometry_msgs::TransformStamped static_transform_stamped;
    static_transform_stamped.header.stamp = ros::Time::now();
    static_transform_stamped.header.frame_id = "cam_2_depth_optical_frame";
    static_transform_stamped.child_frame_id = "cam_1_depth_optical_frame";

    // Translation Vector
    static_transform_stamped.transform.translation.x = T(0,3);
    static_transform_stamped.transform.translation.y = T(1,3);
    static_transform_stamped.transform.translation.z = T(2,3);
    
    // Rotation
    static_transform_stamped.transform.rotation.x = quaternion.x();
    static_transform_stamped.transform.rotation.y = quaternion.y();
    static_transform_stamped.transform.rotation.z = quaternion.z();
    static_transform_stamped.transform.rotation.w = quaternion.w();

    // Broadcast Transformation to /tf ros topic
    static_broadcaster.sendTransform(static_transform_stamped);

    // Needed to properly publish to /tf topic
    ros::spin();

  }

  // ------------------------------------------------------
  //  END
  // ------------------------------------------------------

  return 0;

}
