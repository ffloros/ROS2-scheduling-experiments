#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/string.hpp"
#include <chrono>
#include <string>
#include "../../periodic_task_code.hpp"
#include <unistd.h>
#include <sched.h>

class PeriodicTask1 : public PeriodicTask
{
public:
	PeriodicTask1() : PeriodicTask::PeriodicTask("periodic_task_1") 
	{

		PeriodicTaskParameters parameters;
		parameters.period = 200;
		parameters.priority = 1;
		parameters.deadline = 300;
		parameters.topic = "sensor_1_data";
		registerInScheduler(parameters);

		PeriodicTask1_publisher_ = this->create_publisher<std_msgs::msg::String>("sensor_1_data", 1);
	}

private:
	int executeTask(std::string *error) override 
	{
		unsigned long previous = 0;
		int miliseconds_passed = 0;

		// Dummy busywaiting in ms
		while(true) {
			unsigned long now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
			if (now - previous >= 5) {
				miliseconds_passed = miliseconds_passed + 5;
				previous = now;
			}
			if (miliseconds_passed > 50) {
				break;
			}
		}

		//Data to publish
		auto message = std_msgs::msg::String();
		message.data  = "dummy_sensor_data";
		PeriodicTask1_publisher_->publish(message);

		return 0;
	}

	rclcpp::Publisher<std_msgs::msg::String>::SharedPtr PeriodicTask1_publisher_;
	int producer_counter=0; 
};

int main(int argc, char * argv[])
{
	rclcpp::init(argc, argv);
	rclcpp::spin(std::make_shared<PeriodicTask1>());
	rclcpp::shutdown();

	return 0;
}