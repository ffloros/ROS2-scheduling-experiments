#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/string.hpp"
#include <chrono>
#include <string>
#include "../../periodic_task_code.hpp"
#include <unistd.h>

class PeriodicTask2 : public PeriodicTask
{
public:
	PeriodicTask2() : PeriodicTask::PeriodicTask("periodic_task_2") 
	{

		PeriodicTaskParameters parameters;
		parameters.period = 600;
		parameters.priority = 2;
		parameters.deadline = 500;
		registerInScheduler(parameters);
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
			if (miliseconds_passed > 100) {
				break;
			}
		}

		return 0;	
	}
};

int main(int argc, char * argv[])
{
	rclcpp::init(argc, argv);
	rclcpp::spin(std::make_shared<PeriodicTask2>());
	rclcpp::shutdown();

	return 0;
}
