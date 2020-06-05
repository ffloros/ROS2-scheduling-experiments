#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/string.hpp"
#include <chrono>
#include <string>
#include <sys/mman.h>		
#include <sched.h>
#include <sstream>
#include <iterator>

using std::placeholders::_1;

struct PeriodicTaskParameters {
	unsigned int period;
	unsigned int priority;
	unsigned int deadline = 0;
	std::string topic;
};

class PeriodicTask : public rclcpp::Node
{
public:
	PeriodicTask(const std::string & node_name) : Node(node_name) 
	{		
#ifdef SINGLE_CORE_DEBUG
		cpu_set_t  mask;
    	CPU_ZERO(&mask);
    	CPU_SET(0, &mask);
    	sched_setaffinity(0, sizeof(mask), &mask);
#endif
		
		current_system_level_priority = 1;

		node_execution_subscription_ = this->create_subscription<std_msgs::msg::String>("node_execution", 
			10, 
			std::bind(&PeriodicTask::checkExecutionRequest, this, _1));
		
		std::string node_specific_execution_topic_name = "node_execution_";
		node_specific_execution_topic_name.append(node_name);
		node_specific_execution_subscription_ = this->create_subscription<std_msgs::msg::String>(node_specific_execution_topic_name, 
			10, 
			std::bind(&PeriodicTask::checkExecutionRequest, this, _1));
		priority_reduction_subscription_ = this->create_subscription<std_msgs::msg::String>("priority_reduction", 
			10, 
			std::bind(&PeriodicTask::priorityReductionRequest, this, _1));
		node_registration_publisher_ = this->create_publisher<std_msgs::msg::String>("node_registration", 10);
		node_execution_completion_publisher_ = this->create_publisher<std_msgs::msg::String>("node_execution_completion", 10);
	}

	void registerInScheduler(PeriodicTaskParameters & periodic_task_parameters)
	{
		periodic_task_parameters_ = periodic_task_parameters;
		register_timer_ = this->create_wall_timer(std::chrono::milliseconds(1000), std::bind(&PeriodicTask::delayedRegisterInScheduler, this));
	}

	virtual int executeTask(std::string *error)
	{
		// override this function
	}


private:
	void delayedRegisterInScheduler()
	{
		auto message = std_msgs::msg::String();
		message.data = "";
		message.data.append(this->get_name());	// name
		message.data.append(" "); message.data.append(std::to_string(periodic_task_parameters_.period));
		message.data.append(" "); message.data.append(std::to_string(periodic_task_parameters_.priority));
		message.data.append(" "); message.data.append(std::to_string(periodic_task_parameters_.deadline));
		if (!periodic_task_parameters_.topic.empty()) {
			message.data.append(" "); message.data.append(periodic_task_parameters_.topic);
		}

		node_registration_publisher_->publish(message);

		RCLCPP_INFO(this->get_logger(), "Registration sent");

		register_timer_->cancel();
	}

	void checkExecutionRequest(const std_msgs::msg::String::SharedPtr msg)
	{
		std::istringstream iss(msg->data.c_str());
		std::vector<std::string> execution_parameters(std::istream_iterator<std::string>{iss}, 
		std::istream_iterator<std::string>());

		if (execution_parameters[0] == this->get_name()) 
		{
			RCLCPP_INFO(this->get_logger(), "[%lu] executing", std::chrono::system_clock::now());

			setExecutingPriorityOnSystemLevel(std::stoi(execution_parameters[1]));
			std::string error;
			int execution_result = executeTask(&error);
			setDefaultPriorityOnSystemLevel();

			auto message = std_msgs::msg::String();
			message.data = this->get_name();

			// unused reporting of failed or successful execution
			//
			// if (execution_result == 1) {
			// 	message.data = "";
			// 	message.data.append(this->get_name());
			// 	message.data.append(" "); message.data.append("failed");
			// 	message.data.append(" "); message.data.append(error);
			// } else {
			// 	message.data = "";
			// 	message.data.append(this->get_name());
			// 	message.data.append(" "); message.data.append("successful");
			// }
			
			node_execution_completion_publisher_->publish(message);
		}
	}

	void priorityReductionRequest(const std_msgs::msg::String::SharedPtr msg)
	{
		reducePriorityOnSystemLevel();
	}

	void setDefaultPriorityOnSystemLevel()
	{
		current_system_level_priority = 98;
		//RCLCPP_INFO(this->get_logger(), "Default Priority is %d", current_system_level_priority);
		sched_param pri = {current_system_level_priority}; 
  		if (sched_setscheduler(0, SCHED_FIFO, &pri) == -1) {
  			throw std::runtime_error("unable to set SCHED_FIFO");
  		}
	}

	void setExecutingPriorityOnSystemLevel(int prio)
	{
		current_system_level_priority = prio;
		//RCLCPP_INFO(this->get_logger(), "Executing Priority is %d", current_system_level_priority);
		sched_param pri = {current_system_level_priority}; 
  		if (sched_setscheduler(0, SCHED_FIFO, &pri) == -1) {
  			throw std::runtime_error("unable to set SCHED_FIFO");
  		}
	}

	void reducePriorityOnSystemLevel()
	{
		if (current_system_level_priority <= 1) {
			RCLCPP_INFO(this->get_logger(), "Priority reduction skipped");
			return;
		}

		current_system_level_priority --;
		//RCLCPP_INFO(this->get_logger(), "Reduced Priority is %d", current_system_level_priority);
		sched_param pri = {current_system_level_priority}; 
  		if (sched_setscheduler(0, SCHED_FIFO, &pri) == -1) {
  			throw std::runtime_error("unable to set SCHED_FIFO");
  		}
	}

	PeriodicTaskParameters periodic_task_parameters_;
	int current_system_level_priority;
	int prio = 1;

	rclcpp::TimerBase::SharedPtr register_timer_;
	rclcpp::Subscription<std_msgs::msg::String>::SharedPtr node_execution_subscription_;
	rclcpp::Subscription<std_msgs::msg::String>::SharedPtr node_specific_execution_subscription_;
	rclcpp::Subscription<std_msgs::msg::String>::SharedPtr priority_reduction_subscription_;
	rclcpp::Publisher<std_msgs::msg::String>::SharedPtr node_registration_publisher_;
	rclcpp::Publisher<std_msgs::msg::String>::SharedPtr node_execution_completion_publisher_;
};
