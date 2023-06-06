cmake_minimum_required(VERSION 3.8)
project(tf2_web_republisher)

# Default to C99
if(NOT CMAKE_C_STANDARD)
  set(CMAKE_C_STANDARD 99)
endif()

# Default to C++17
if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic -Wfatal-errors)
endif()


## Find catkin macros and libraries
## if COMPONENTS list like find_package(catkin REQUIRED COMPONENTS xyz)
## is used, also find other catkin packages
find_package(ament_cmake REQUIRED)
find_package(tf2_ros REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(tf2_geometry_msgs REQUIRED)
find_package(std_msgs REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rosidl_default_generators REQUIRED)



#######################################
## Declare ROS messages and services ##
#######################################

rosidl_generate_interfaces(${PROJECT_NAME}
  action/TFSubscription.action
  msg/TFArray.msg
  services/RepublishTFs.srv
  DEPENDENCIES geometry_msgs std_msgs
)

###################################################
## Declare things to be passed to other projects ##
###################################################

## LIBRARIES: libraries you create in this project that dependent projects also need
## CATKIN_DEPENDS: catkin_packages dependent projects also need
## DEPENDS: system dependencies of this project that dependent projects also need
set(dependencies
  tf2_ros
  tf2_geometry_msgs
  geometry_msgs
  std_msgs
  rclcpp
)

###########
## Build ##
###########

## Specify additional locations of header files
include_directories(
  include
)

## Declare a cpp executables
add_executable(${PROJECT_NAME}_node
  src/tf_web_republisher.cpp
)
ament_target_dependencies(${PROJECT_NAME}_node
  ${dependencies}
)

rosidl_get_typesupport_target(cpp_typesupport_target ${PROJECT_NAME} "rosidl_typesupport_cpp")
target_link_libraries(${PROJECT_NAME}_node "${cpp_typesupport_target}") 

#############
## Install ##
#############

## Mark executables and/or libraries for installation
install(TARGETS
  ${PROJECT_NAME}_node
  RUNTIME DESTINATION lib/${PROJECT_NAME}
)

#############
## Rostest ##
#############
#if(BUILD_TESTING)
#  find_package(rostest REQUIRED)
#
#  add_rostest(test/test_tf2_web_republisher.test)
#endif()

ament_package()
