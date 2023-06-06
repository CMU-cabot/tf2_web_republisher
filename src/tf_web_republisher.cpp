/*********************************************************************
 *
 *  Copyright (c) 2014, Willow Garage, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the Willow Garage nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.

 *  Author: Julius Kammerl (jkammerl@willowgarage.com)
 *
 */

#include <sstream>
#include <thread>

#include <rclcpp/rclcpp.hpp>
#include <tf2_ros/transform_listener.h>
#include <tf2_ros/buffer.h>
#include <tf2/utils.h>
#include <geometry_msgs/msg/transform_stamped.hpp>

#include "rclcpp_action/rclcpp_action.hpp"
#include "tf2_web_republisher/action/tf_subscription.hpp"
#include "tf2_web_republisher/srv/republish_t_fs.hpp"
#include "tf2_web_republisher/msg/tf_array.hpp"

#include "tf_pair.h"

using namespace std::placeholders;

class TFRepublisher: public rclcpp::Node
{
protected:
  using TFArray = tf2_web_republisher::msg::TFArray;
  using TFSubscription = tf2_web_republisher::action::TFSubscription;
  using RepublishTFs = tf2_web_republisher::srv::RepublishTFs;
  using TFSubscriptionGoal = tf2_web_republisher::action::TFSubscription::Goal;
  using GoalHandle = rclcpp_action::ServerGoalHandle<TFSubscription>;

  typedef rclcpp_action::Server<TFSubscription>::SharedPtr TFTransformServer;
  using RepublishTFsRequest = tf2_web_republisher::srv::RepublishTFs::Request;
  using RepublishTFsResponse = tf2_web_republisher::srv::RepublishTFs::Response;
  

  //rclcpp::Node node_;
  //ros::NodeHandle nh_;
  //ros::NodeHandle priv_nh_;

  TFTransformServer as_;
  rclcpp::Service<RepublishTFs>::SharedPtr tf_republish_service_;

  // base struct that holds information about the TFs
  // a client (either Service or Action) has subscribed to
  struct ClientInfo
  {
    std::vector<TFPair> tf_subscriptions_;
    unsigned int client_ID_;
    rclcpp::TimerBase::SharedPtr  timer_;
  };

  // struct for Action client info
  struct ClientGoalInfo : ClientInfo
  {
   std::shared_ptr<GoalHandle> handle;
  };

  // struct for Service client info
  struct ClientRequestInfo : ClientInfo
  {
    rclcpp::Publisher<TFArray>::SharedPtr pub_;
    std::chrono::nanoseconds unsub_timeout_;
    rclcpp::TimerBase::SharedPtr unsub_timer_;
  };

  std::list<std::shared_ptr<ClientGoalInfo> > active_goals_;
  std::mutex goals_mutex_;

  std::list<std::shared_ptr<ClientRequestInfo> > active_requests_;
  std::mutex requests_mutex_;

  tf2_ros::Buffer * tf_buffer_;
  tf2_ros::TransformListener * tf_listener_;
  std::mutex tf_buffer_mutex_;

  unsigned int client_ID_count_;

public:

  TFRepublisher() :
      Node("tf2_web_republisher"),
      client_ID_count_(0)
  {
    /*
    as_(nh_,
        name,
        std::bind(&TFRepublisher::goalCB, this, _1),
        std::bind(&TFRepublisher::cancelCB, this, _1),
        false),
    priv_nh_("~"),
    tf_buffer_(),
    tf_listener_(tf_buffer_),
    */
    tf_buffer_ = new tf2_ros::Buffer(this->get_clock());
    tf_listener_ = new tf2_ros::TransformListener(*tf_buffer_);

    as_ = rclcpp_action::create_server<TFSubscription>(
        this,
        "tf2_web_republisher",
        std::bind(&TFRepublisher::goalCB, this, _1, _2),
        std::bind(&TFRepublisher::cancelCB, this, _1),
        std::bind(&TFRepublisher::acceptCB, this, _1));


    tf_republish_service_ = this->create_service<RepublishTFs>(
        "republish_tfs",
        std::bind(&TFRepublisher::requestCB, this, _1, _2));
    RCLCPP_INFO(this->get_logger(), "TFRepublisher constructor completed");
  }

  ~TFRepublisher() {}

  rclcpp_action::CancelResponse cancelCB(
      const std::shared_ptr<GoalHandle> gh)
  {
    RCLCPP_DEBUG(this->get_logger(), "GoalHandle canceled");
    
    // search for goal handle and remove it from active_goals_ list
    for(std::list<std::shared_ptr<ClientGoalInfo> >::iterator it = active_goals_.begin(); it != active_goals_.end();)
    {
      ClientGoalInfo& info = **it;
      if(info.handle == gh)
      {
        it = active_goals_.erase(it);
        info.timer_->cancel();
        auto msg = std::make_shared<TFSubscription::Result>();
        gh->canceled(msg);
        return rclcpp_action::CancelResponse::ACCEPT;
      }
      else
        ++it;
    }
    return rclcpp_action::CancelResponse::REJECT;
  }

  const std::string cleanTfFrame( const std::string frame_id ) const
  {
    if ( frame_id[0] == '/' )
    {
      return frame_id.substr(1);
    }
    return frame_id;
  }

  /**
   * Set up the contents of \p tf_subscriptions_ in
   * a ClientInfo struct
   */
  void setSubscriptions(std::shared_ptr<ClientInfo> info,
                        const std::vector<std::string>& source_frames,
                        const std::string& target_frame_,
                        float angular_thres,
                        float trans_thres) const
  {
    std::size_t request_size_ = source_frames.size();
    info->tf_subscriptions_.resize(request_size_);

    for (std::size_t i=0; i<request_size_; ++i )
    {
      TFPair& tf_pair = info->tf_subscriptions_[i];

      std::string source_frame = cleanTfFrame(source_frames[i]);
      std::string target_frame = cleanTfFrame(target_frame_);

      tf_pair.setSourceFrame(source_frame);
      tf_pair.setTargetFrame(target_frame);
      tf_pair.setAngularThres(angular_thres);
      tf_pair.setTransThres(trans_thres);
    }
  }

  rclcpp_action::GoalResponse goalCB(
    const rclcpp_action::GoalUUID /* unused */,
    std::shared_ptr<const TFSubscription::Goal> goal)
      //void goalCB(GoalHandle gh)
  {
    RCLCPP_DEBUG(this->get_logger(), "Received goal request with frame: %s", goal->target_frame.c_str());
    RCLCPP_DEBUG(this->get_logger(), "GoalHandle request received");
    return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
  }    

  void acceptCB(const std::shared_ptr<GoalHandle> gh) {
    // accept new goals
    //gh.setAccepted();

    // get goal from handle
    //const tf2_web_republisher::TFSubscriptionGoal::ConstPtr& goal = gh.getGoal();

    // generate goal_info struct
    std::shared_ptr<ClientGoalInfo> goal_info = std::make_shared<ClientGoalInfo>();
    
    goal_info->handle = gh;
    goal_info->client_ID_ = client_ID_count_++;
    
    auto goal = gh->get_goal();

    // add the tf_subscriptions to the ClientGoalInfo object
    setSubscriptions(goal_info,
                     goal->source_frames,
                     goal->target_frame,
                     goal->angular_thres,
                     goal->trans_thres);

    goal_info->timer_ = this->create_wall_timer(std::chrono::duration<float>(1.0 / goal->rate),
                                                [this, goal_info]() -> void { this->processGoal(goal_info); });    

    {
      // add new goal to list of active goals/clients
      active_goals_.push_back(goal_info);
    }
  }

  
  void requestCB(const std::shared_ptr<RepublishTFsRequest> req,
                 std::shared_ptr<RepublishTFsResponse>      res)
  {
    RCLCPP_DEBUG(this->get_logger(), "RepublishTF service request received");
    // generate request_info struct
    auto request_info = std::make_shared<ClientRequestInfo>();

    request_info->client_ID_ = client_ID_count_;
    std::stringstream topicname;
    topicname << "tf_repub_" << client_ID_count_++;

    request_info->pub_ = this->create_publisher<tf2_web_republisher::msg::TFArray>(topicname.str(), 10);

    // add the tf_subscriptions to the ClientGoalInfo object
    setSubscriptions(request_info,
                     req->source_frames,
                     req->target_frame,
                     req->angular_thres,
                     req->trans_thres);

    request_info->unsub_timeout_ = rclcpp::Duration(req->timeout).to_chrono<std::chrono::nanoseconds>();
    request_info->unsub_timer_ = this->create_wall_timer(request_info->unsub_timeout_,
                                                        [this, request_info]() -> void {this->unadvertiseCB(request_info);});

    request_info->timer_ = this->create_wall_timer(std::chrono::duration<float>(1.0 / req->rate),
                                                    [this, request_info]() -> void { this->processRequest(request_info); });

    {
      // add new request to list of active requests
      active_requests_.push_back(request_info);
    }
    res->topic_name = request_info->pub_->get_topic_name();
    RCLCPP_INFO(this->get_logger(), "Publishing requested TFs on topic %s", res->topic_name.c_str());
  }

  void unadvertiseCB(std::shared_ptr<ClientRequestInfo> request_info)
  {
    RCLCPP_INFO(this->get_logger(), "No subscribers on tf topic for request %d for %.2f seconds. Unadvertising topic: %s",
                request_info->client_ID_, request_info->unsub_timeout_.count()/1e9f, request_info->pub_->get_topic_name());
    request_info->pub_ = nullptr;
    request_info->unsub_timer_->cancel();
    request_info->timer_->cancel();

    // search for ClientRequestInfo struct and remove it from active_requests_ list
    for(std::list<std::shared_ptr<ClientRequestInfo> >::iterator it = active_requests_.begin(); it != active_requests_.end(); ++it)
    {
      ClientRequestInfo& info = **it;
      if(info.pub_ == request_info->pub_)
      {
        active_requests_.erase(it);
        return;
      }
    }
  }

  void updateSubscriptions(std::vector<TFPair>& tf_subscriptions,
                           std::vector<geometry_msgs::msg::TransformStamped>& transforms)
  {
    // iterate over tf_subscription vector
    std::vector<TFPair>::iterator it ;
    std::vector<TFPair>::const_iterator end = tf_subscriptions.end();

    for (it=tf_subscriptions.begin(); it!=end; ++it)
    {
      geometry_msgs::msg::TransformStamped transform;

      try
      {
        // protecting tf_buffer
        // lookup transformation for tf_pair
        transform = tf_buffer_->lookupTransform(it->getTargetFrame(),
                                                it->getSourceFrame(),
                                                rclcpp::Time());

        // If the transform broke earlier, but worked now (we didn't get
        // booted into the catch block), tell the user all is well again
        if (!it->is_okay)
        {
          it->is_okay = true;
          RCLCPP_INFO(this->get_logger(), "Transform from %s to %s is working again at time %d.%d",
                      it->getSourceFrame().c_str(), it->getTargetFrame().c_str(), transform.header.stamp.sec, transform.header.stamp.nanosec);
        }
        // update tf_pair with transformtion
        it->updateTransform(transform);
      }
      catch (tf2::TransformException &ex)
      {
        // Only log an error if the transform was okay before
        if (it->is_okay)
        {
          it->is_okay = false;
          RCLCPP_ERROR(this->get_logger(), "%s", ex.what());
        }
      }

      // check angular and translational thresholds
      if (it->updateNeeded())
      {
        transform.header.stamp = this->get_clock()->now();
        transform.header.frame_id = it->getTargetFrame();
        transform.child_frame_id = it->getSourceFrame();

        // notify tf_subscription that a network transmission has been triggered
        it->transmissionTriggered();

        // add transform to the array
        transforms.push_back(transform);
      }
    }
  }

  void processGoal(std::shared_ptr<ClientGoalInfo> goal_info)
  {
    auto feedback = std::make_shared<tf2_web_republisher::action::TFSubscription::Feedback>();

    updateSubscriptions(goal_info->tf_subscriptions_,
                        feedback->transforms);

    if (feedback->transforms.size() > 0)
    {
      // publish feedback
      goal_info->handle->publish_feedback(feedback);
      RCLCPP_DEBUG(this->get_logger(), "Client %d: TF feedback published:", goal_info->client_ID_);
    } else
    {
      RCLCPP_DEBUG(this->get_logger(), "Client %d: No TF frame update needed:", goal_info->client_ID_);
    }
  }

  void processRequest(std::shared_ptr<ClientRequestInfo> request_info)
  {
    if (request_info->pub_->get_subscription_count() == 0)
    {
      request_info->unsub_timer_->reset();
    }
    else
    {
      request_info->unsub_timer_->cancel();
    }

    auto array_msg = std::make_shared<tf2_web_republisher::msg::TFArray>();
    updateSubscriptions(request_info->tf_subscriptions_,
                        array_msg->transforms);

    if (array_msg->transforms.size() > 0)
    {
      // publish TFs
      request_info->pub_->publish(*array_msg);
      RCLCPP_DEBUG(this->get_logger(), "Request %d: TFs published:", request_info->client_ID_);
    }
    else
    {
      RCLCPP_DEBUG(this->get_logger(), "Request %d: No TF frame update needed:", request_info->client_ID_);
    }
  }
};

int main(int argc, char **argv)
{
  rclcpp::init(argc, argv);  
  auto tf2_web_republisher = std::make_shared<TFRepublisher>();
  rclcpp::spin(tf2_web_republisher);
  rclcpp::shutdown();
  return 0;
}
