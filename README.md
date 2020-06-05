# ROS2-scheduling-experiments
Experimental ways of extending or improving the ROS2 nodes scheduling methods

## Scheduler as a node
Adding new scheduling policies is enabled through a **simple_scheduler** which runs as separate node.
The scheduler node runs on highest real-time system level priority and triggers execution of registered periodic nodes by sending a specific message on a *node_execution* topic. It also manipulates their real-time system level priorities to ensure correct execution order.

### Getting started
This is how you could traditionally set a system level priority (from the real-time range) and ensure periodic execution of a node.
```
class SomePeriodicNode : public rclcpp::Node
{
public:
	SomePeriodicNode() : rclcpp::Node("some_periodic_node") 
	{
		sched_param pri = {3}; 
  		if (sched_setscheduler(0, SCHED_FIFO, &pri) == -1) {
  			throw std::runtime_error("unable to set SCHED_FIFO");
  		}

  		node_publisher_ = this->create_publisher<std_msgs::msg::String>("node_data", 1);

  		timer_ = this->create_wall_timer(1000ms, std::bind(&SomePeriodicNode::executeTask, this));
  		executeTask();
	}

private:
	void executeTask() 
	{
		...
```
With the use of the **simple_scheduler** the implementation changes as follows.
```
class SomePeriodicNode : public PeriodicTask
{
public:
	SomePeriodicNode() : PeriodicTask::PeriodicTask("some_periodic_node") 
	{

		PeriodicTaskParameters parameters;
		parameters.period = 1000;
		parameters.priority = 3;
		parameters.deadline = 2000;
		registerInScheduler(parameters);
	}

private:
	int executeTask(std::string *error) override 
	{
		...
```
For now the implementation assumes that the scheduler node and the periodic nodes are launched separately. Make sure you first launch the scheduler node. Once you launch the other nodes, they will register themselves in the scheduler and start execution with the appropriate period, priority etc. parameters. 