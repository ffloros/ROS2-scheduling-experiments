#include "rclcpp/rclcpp.hpp" 
#include "rclcpp/parameter.hpp"
#include "std_msgs/msg/string.hpp"
#include "std_msgs/msg/float32.hpp"
#include <climits>
#include <chrono>
#include <string>
#include <sstream>
#include <iterator>
#include <vector>
#include <unordered_map>
#include <sys/time.h>
#include <stdexcept>
#include <iostream>
#include <iomanip>
#include <map>
#include <sys/mman.h>		
#include <sched.h>
#include <algorithm>

#define SCHEDULING_POLICY_PARAMETER_NAME "scheduling_policy"
#define MAX_NODES_COUNT 20

using std::placeholders::_1;

class SimpleScheduler : public rclcpp::Node
{
	struct ScheduledNode {
		std::string name;
		int period;
		int priority;
		int dynamic_priority;
		int deadline;
		unsigned long next_release;
		bool is_executing;
		bool use_dynamic_prio;
		std::string topic;
	};

	enum ExecutionRequestDeliveryMethod {
		SingleTopic, 			// default
		NodeSpecificTopic,
	};

public: 
	enum SchedulingPolicy: int {
		NP_FixedPriority,		// default
		NP_EarliestDeadlineFirst,
		P_FixedPriority,
		P_EarliestDeadlineFirst
	};

	SimpleScheduler() : Node("simple_scheduler")
	{
#ifdef SINGLE_CORE_DEBUG
		// for test purposes, ensure execution on a single core
		cpu_set_t  mask;
    	CPU_ZERO(&mask);
    	CPU_SET(0, &mask);
    	sched_setaffinity(0, sizeof(mask), &mask);
#endif
    	
		// lock all cached memory into RAM
		mlockall(MCL_FUTURE);

		// set highest possible real-time priority for the process on the system level
		// and set the system scheduling policy to FIFO
  		sched_param pri = {99}; 
  		if (sched_setscheduler(0, SCHED_FIFO, &pri) == -1) {
  			throw std::runtime_error("unable to set SCHED_FIFO");
  		}

  		// reserve a specific amount of memory for nodes and publishers
		scheduled_nodes_.reserve(MAX_NODES_COUNT);
		node_specific_execution_publishers_map_.reserve(MAX_NODES_COUNT);

		// initialise yaml parametrised class parameters 
		int scheduling_policy_parameter_value;
		this->declare_parameter(SCHEDULING_POLICY_PARAMETER_NAME);
  		this->get_parameter_or(SCHEDULING_POLICY_PARAMETER_NAME, scheduling_policy_parameter_value, 0);
  		switch (scheduling_policy_parameter_value)
  		{
  			case 0: { scheduling_policy_ = NP_FixedPriority; break;}
  			case 1: { scheduling_policy_ = NP_EarliestDeadlineFirst; break;}
  			case 2: { scheduling_policy_ = P_FixedPriority; break;}
  			case 3: { scheduling_policy_ = P_EarliestDeadlineFirst; break;}
  			default: scheduling_policy_ = NP_FixedPriority;
  		}

		// initialise other class parameters
		scheduled_nodes_count_ = 0;

		execution_request_delivery_method_ = SingleTopic;

		node_registration_subscription_ = this->create_subscription<std_msgs::msg::String>("node_registration", 
			10, 
			std::bind(&SimpleScheduler::node_registration_callback, this, _1));
		node_execution_completion_subscription_ = this->create_subscription<std_msgs::msg::String>("node_execution_completion", 
			10, 
			std::bind(&SimpleScheduler::node_execution_completion_callback, this, _1));
		node_execution_publisher_ = this->create_publisher<std_msgs::msg::String>("node_execution", 10);
		priority_reduction_publisher_ = this->create_publisher<std_msgs::msg::String>("priority_reduction", 10);

		// log start info
		RCLCPP_INFO(this->get_logger(), 
			"Scheduler started (scheduling policy: %d, execution request delivery method: %d)", 
			scheduling_policy_, execution_request_delivery_method_);
	}

	void set_scheduling_policy(SchedulingPolicy new_scheduling_policy)
	{
		scheduling_policy_ = new_scheduling_policy;
	}

private:
	void scheduler()
	{
		scheduler_from_node_completion();
	}
	void scheduler_from_node_completion(std::string completion_data = "")
	{
		//debug
		sched_getscheduler(0);

		// auto scheduler_start_time = std::chrono::steady_clock::now();
		if (completion_data != "")
		{

			std::istringstream iss(completion_data.c_str());
			std::vector<std::string> completion_parameters(std::istream_iterator<std::string>{iss}, 
				std::istream_iterator<std::string>());
			std::string completed_node_name = completion_parameters[0];
			std::string completion_result = completion_parameters[1];
			std::string completion_error ;

			if(completion_parameters.size() == 3){
				completion_error = completion_parameters[2];
			}

			for (int i = 0; i < scheduled_nodes_count_; i++)
			{
				ScheduledNode *node = &scheduled_nodes_[i];
				if (node->name == completed_node_name)
				{
					node->is_executing = false;
					node->use_dynamic_prio = false;
					node->dynamic_priority = 1;
				}
			}
		}

		switch (scheduling_policy_) 
		{
			case NP_FixedPriority : { scheduler_NP_FP(); break; }
			case NP_EarliestDeadlineFirst : { scheduler_NP_EDF(); break; }
			case P_FixedPriority : { scheduler_P_FP(); break; }
			case P_EarliestDeadlineFirst : { scheduler_P_EDF(); break; }
			default: throw std::runtime_error("scheduling_policy_ had incorrect value");
		}
	}

	void scheduler_NP_EDF()
	{
		if (timer_)
		{
			timer_->cancel();
		}

		if (scheduled_nodes_count_ == 0) 
		{ 
			return;
		}

		unsigned long now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		ScheduledNode *earliest_deadline_node = NULL;
		auto earliest_deadline = ULONG_MAX;
		auto next_release = ULONG_MAX;

		for (int i = 0; i < scheduled_nodes_count_; i++)
		{	
			ScheduledNode *node = &scheduled_nodes_[i];
			auto node_deadline = node->next_release + node->deadline;

			if (node->next_release <= now)
			{
				if (node_deadline < earliest_deadline ||
					(node_deadline == earliest_deadline && node->priority > earliest_deadline_node->priority)) 
				{
					earliest_deadline_node = node;
					earliest_deadline = node_deadline;
				}
			}
			else if (node->next_release < next_release)
			{
				next_release = node->next_release;
			}
		}

		if (earliest_deadline_node != NULL)
		{
			earliest_deadline_node->next_release = earliest_deadline_node->next_release + earliest_deadline_node->period;
			send_execution_request(earliest_deadline_node->name, 1);
		}
		else
		{
			auto time_untill_next_release = next_release - now;
			timer_ = this->create_wall_timer(std::chrono::milliseconds(time_untill_next_release), std::bind(&SimpleScheduler::scheduler, this));
		}
	}

	void scheduler_NP_FP()
	{		
		if (timer_)
		{
			timer_->cancel();
		}

		if (scheduled_nodes_count_ == 0) 
		{ 
			return;
		}

    	unsigned long now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		ScheduledNode *selected_scheduled_node = NULL;
		auto next_release = ULONG_MAX;
		for (int i = 0; i < scheduled_nodes_count_; i++) 
		{
			ScheduledNode *node = &scheduled_nodes_[i];
			if (node->next_release <= now)
			{
				if (selected_scheduled_node == NULL ||
					selected_scheduled_node->priority < node->priority) 
				{
					selected_scheduled_node = node;
				}
			}
			else if (node->next_release < next_release)
			{
				next_release = node->next_release;
			}
		}

		if (selected_scheduled_node != NULL) 
		{
			selected_scheduled_node->next_release = selected_scheduled_node->next_release + selected_scheduled_node->period;
			send_execution_request(selected_scheduled_node->name, 1);
		}
		else 
		{
			auto time_untill_next_release = next_release - now;
			timer_ = this->create_wall_timer(std::chrono::milliseconds(time_untill_next_release), std::bind(&SimpleScheduler::scheduler, this));
		}
	}

	static bool fixed_priority_comparison(ScheduledNode const& lhs, ScheduledNode const& rhs) 
	{
		int lhs_prio = lhs.priority;
		int rhs_prio = rhs.priority;
		if (lhs.use_dynamic_prio) {
			lhs_prio = lhs.dynamic_priority;
		}
		if (rhs.use_dynamic_prio) {
			rhs_prio = rhs.dynamic_priority;
		}

		return lhs_prio > rhs_prio;
	}

	void scheduler_P_FP()
	{
		if (timer_)
		{
			timer_->cancel();
		}

		if (scheduled_nodes_count_ == 0) 
		{ 
			RCLCPP_INFO(this->get_logger(), "No nodes to schedule");
			return;
		}

    	unsigned long now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

    	std::sort(
    		scheduled_nodes_.begin(), 
    		scheduled_nodes_.end(), 
    		&fixed_priority_comparison);

		ScheduledNode *selected_scheduled_node = NULL;
		auto next_release = ULONG_MAX;

		for (int i = 0; i < scheduled_nodes_count_; i++) 
		{		
			ScheduledNode *node = &scheduled_nodes_[i];

			if (node->is_executing == true)
				break;

			// if node execution time is now or in the past it should become a candidate for execution
			if (selected_scheduled_node == NULL && node->next_release <= now)
			{
				selected_scheduled_node = node;
				break;
			}
		}

		for (int i = 0; i < scheduled_nodes_count_; i++) 
		{
			ScheduledNode *node = &scheduled_nodes_[i];

			if (node->next_release < next_release && node->next_release > now)
			{
				next_release = node->next_release;
			}
		}

		auto time_untill_next_release = next_release - now;
		RCLCPP_INFO(this->get_logger(),"next_release: %lu, now: %lu, time_untill_next_release: %lu", next_release, now, time_untill_next_release);
		if (next_release == ULONG_MAX) 
		{
			RCLCPP_INFO(this->get_logger(), "no future releases");
		}
		else 
		{
			timer_ = this->create_wall_timer(std::chrono::milliseconds(time_untill_next_release), std::bind(&SimpleScheduler::scheduler, this));
		}
		if (selected_scheduled_node == NULL)
		{
			RCLCPP_INFO(this->get_logger(), "no selected_scheduled_node");
		}
		
		if (selected_scheduled_node != NULL) 
		{
			// send_priority_reduction_request();
			selected_scheduled_node->next_release = selected_scheduled_node->next_release + selected_scheduled_node->period;
			selected_scheduled_node->is_executing = true;

			int node_prio = selected_scheduled_node->priority;
			if (selected_scheduled_node->use_dynamic_prio) {
				node_prio = selected_scheduled_node->dynamic_priority;
				RCLCPP_INFO(this->get_logger(), "I sent the dynamic prio %d on node %s", selected_scheduled_node->dynamic_priority, selected_scheduled_node->name);
			}

			RCLCPP_INFO(this->get_logger(), "Node %s was dispatched with prio %d and its normal prio is %d", selected_scheduled_node->name.c_str(),node_prio,selected_scheduled_node->priority);
			send_execution_request(selected_scheduled_node->name, node_prio);
		}
	}

	void scheduler_P_EDF()
	{
		if (timer_)
		{
			timer_->cancel();
		}

		if (scheduled_nodes_count_ == 0) 
		{ 
			RCLCPP_INFO(this->get_logger(), "No nodes to schedule");
			return;
		}

    	unsigned long now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		
		ScheduledNode *selected_scheduled_node = NULL;
		auto selected_scheduled_node_deadline = ULONG_MAX;
		
		ScheduledNode *earliest_executing_node = NULL;
		auto earliest_executing_node_deadline = ULONG_MAX;
		
		auto earliest_deadline = ULONG_MAX;
		auto next_release = ULONG_MAX;

		for (int i = 0; i < scheduled_nodes_count_; i++) 
		{
			ScheduledNode *node = &scheduled_nodes_[i];

			auto node_previous_deadline = node->next_release - node->period + node->deadline;
			auto node_next_deadline = node->next_release + node->deadline;

			// if currently executed node has higher priority than the node selected for execution than preemption should not happen
			if (node->is_executing && 
				(earliest_executing_node_deadline > node_previous_deadline ||
					earliest_executing_node_deadline == node_previous_deadline && earliest_executing_node->priority < node->priority))
			{
				earliest_executing_node = node;
				earliest_executing_node_deadline = node_previous_deadline;
			}

			// if node execution time is now or in the past it should become a candidate for execution
			if (node->next_release <= now)
			{
				if (selected_scheduled_node == NULL || selected_scheduled_node_deadline > node_next_deadline ||
						(selected_scheduled_node_deadline == node_next_deadline && selected_scheduled_node->priority < node->priority))  
				{
					selected_scheduled_node = node;
					selected_scheduled_node_deadline = node_next_deadline;
				}
			}

			// selecting a new preemptive scheduler release time
			if (node->next_release < next_release && node->next_release > now)
			{
				next_release = node->next_release;
			}
		}

		if (selected_scheduled_node != NULL &&
			(selected_scheduled_node_deadline > earliest_executing_node_deadline ||
				(selected_scheduled_node_deadline == earliest_executing_node_deadline && selected_scheduled_node->priority < earliest_executing_node->priority)))
		{
			selected_scheduled_node = NULL;
		}

		auto time_untill_next_release = next_release - now;
		RCLCPP_INFO(this->get_logger(),"next_release: %lu, now: %lu, time_untill_next_release: %lu", next_release, now, time_untill_next_release);
		if (next_release == ULONG_MAX) 
		{
			RCLCPP_INFO(this->get_logger(), "no future releases");
		}
		else 
		{
			timer_ = this->create_wall_timer(std::chrono::milliseconds(time_untill_next_release), std::bind(&SimpleScheduler::scheduler, this));
		}
		if (selected_scheduled_node == NULL)
		{
			RCLCPP_INFO(this->get_logger(), "no selected_scheduled_node");
		}
		
		if (selected_scheduled_node != NULL) 
		{
			selected_scheduled_node->next_release = selected_scheduled_node->next_release + selected_scheduled_node->period;
			selected_scheduled_node->is_executing = true;
			int priority = 1;
			if (earliest_executing_node != NULL) 
			{
				priority = earliest_executing_node->dynamic_priority + 1;
			}
			selected_scheduled_node->dynamic_priority = priority;

			send_execution_request(selected_scheduled_node->name, selected_scheduled_node->dynamic_priority);
		}
	}

	void send_execution_request(std::string node_name, int priority) 
	{
		switch(execution_request_delivery_method_)
		{
			case SingleTopic: 
			{
				auto message = std_msgs::msg::String();
				message.data = "";
				message.data.append(node_name);	// name
				message.data.append(" "); message.data.append(std::to_string(priority));

				node_execution_publisher_->publish(message);
				break;
			}
			case NodeSpecificTopic:
			{
				auto message = std_msgs::msg::String();
				message.data = "";
				message.data.append(node_name);	// name
				message.data.append(" "); message.data.append(std::to_string(priority));

				node_specific_execution_publishers_map_[node_name]->publish(message);
				break;
			}
			default: throw std::runtime_error("execution_request_delivery_method_ had incorrect value");
		}
	}

	void send_priority_reduction_request()
	{
		auto message = std_msgs::msg::String();
		priority_reduction_publisher_->publish(message);
	}

	void node_registration_callback(const std_msgs::msg::String::SharedPtr msg)
	{
		RCLCPP_INFO(this->get_logger(), "Registration requested");

		std::istringstream iss(msg->data.c_str());
		std::vector<std::string> node_parameters(std::istream_iterator<std::string>{iss}, 
			std::istream_iterator<std::string>());

		ScheduledNode scheduled_node;
		scheduled_node.name = node_parameters[0];
		scheduled_node.period = std::stoi(node_parameters[1]);
		scheduled_node.priority = std::stoi(node_parameters[2]);
		scheduled_node.dynamic_priority = 1; 
		scheduled_node.deadline = std::stoi(node_parameters[3]);
		if (node_parameters.size() > 4) 
		{
			scheduled_node.topic = node_parameters[4];
		}
		unsigned long now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		scheduled_node.next_release = now + scheduled_node.period;
		scheduled_node.is_executing = false;
		scheduled_node.use_dynamic_prio = false;

		scheduled_nodes_.push_back(scheduled_node);
		scheduled_nodes_count_ ++; 

		// Create a node specific publish topic
		std::string node_specific_execution_topic_name = "node_execution_";
		node_specific_execution_topic_name.append(scheduled_node.name);
		rclcpp::Publisher<std_msgs::msg::String>::SharedPtr new_publisher = this->create_publisher<std_msgs::msg::String>(node_specific_execution_topic_name, 10);
		node_specific_execution_publishers_map_[scheduled_node.name] = new_publisher;
		// End of - Create a noce specific publish topic

		RCLCPP_INFO(this->get_logger(), "Registerd node: Name:'%s', Period:'%d'ms, Priority:'%d'", 
			scheduled_node.name.c_str(), scheduled_node.period, scheduled_node.priority);

		scheduler();
	}

	void node_execution_completion_callback(const std_msgs::msg::String::SharedPtr msg) 
	{
		scheduler_from_node_completion(msg->data);
	}

	rclcpp::TimerBase::SharedPtr timer_;
	rclcpp::Subscription<std_msgs::msg::String>::SharedPtr node_registration_subscription_;
	rclcpp::Subscription<std_msgs::msg::String>::SharedPtr node_execution_completion_subscription_;
	rclcpp::Publisher<std_msgs::msg::String>::SharedPtr node_execution_publisher_;
	rclcpp::Publisher<std_msgs::msg::String>::SharedPtr priority_reduction_publisher_;
	std::unordered_map<std::string, rclcpp::Publisher<std_msgs::msg::String>::SharedPtr> node_specific_execution_publishers_map_; // check for memory leak

	std::vector<SimpleScheduler::ScheduledNode> scheduled_nodes_;
	int scheduled_nodes_count_;

	SchedulingPolicy scheduling_policy_;
	ExecutionRequestDeliveryMethod execution_request_delivery_method_;
};

int main(int argc, char * argv[])
{
	rclcpp::init(argc, argv);

	std::shared_ptr<SimpleScheduler> scheduler = std::make_shared<SimpleScheduler>();

	rclcpp::spin(scheduler);
	rclcpp::shutdown();

	return 0;
}
