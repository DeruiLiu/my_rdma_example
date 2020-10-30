/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2 as
* published by the Free Software Foundation;
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#undef PGO_TRAINING
#define PATH_TO_PGO_CONFIG "mix/config.txt"

#include <iostream>
#include <fstream>
#include <sstream>
#include<string>
#include <time.h> 
#include <iomanip>
#include <boost/interprocess/ipc/message_queue.hpp> 
#include <boost/thread.hpp> 
#include <boost/process.hpp>
#include "ns3/core-module.h"
#include "ns3/qbb-helper.h"
#include "ns3/point-to-point-helper.h"
#include "ns3/applications-module.h"
#include "ns3/internet-module.h"
#include "ns3/global-route-manager.h"
#include "ns3/ipv4-static-routing-helper.h"
#include "ns3/broadcom-node.h"
#include "ns3/packet.h"
#include "ns3/error-model.h"
#include "ns3/flow-monitor-helper.h"
#include "ns3/flow-monitor-module.h"
//#include "Python.h"
#include "zmq.h"

using namespace ns3;
using namespace boost::interprocess;

boost::mutex mutex;
boost::mutex switch_mutex;
boost::mutex ECN_flag_mutex;
boost::mutex ECN_mutex;
boost::mutex thread_mutex;
boost::mutex queue_length_mutex;
boost::mutex my_thread_mutex;
namespace boost
{
	void throw_exception(std::exception const & e) // user defined
	{
		return;
	}
}

NS_LOG_COMPONENT_DEFINE("GENERIC_SIMULATION");

bool enable_qcn = true, use_dynamic_pfc_threshold = true, packet_level_ecmp = false, flow_level_ecmp = false;
uint32_t packet_payload_size = 1000, l2_chunk_size = 0, l2_ack_interval = 0, average_flow_size = 0;
double pause_time = 5, simulator_stop_time = 3.01, app_start_time = 1.0, app_stop_time = 9.0;
std::string data_rate, link_delay, topology_file, flow_file, tcp_flow_file, trace_file, trace_output_file, myflow_file;
bool used_port[65536] = { 0 };

double cnp_interval = 50, alpha_resume_interval = 55, rp_timer, dctcp_gain = 1 / 16, np_sampling_interval = 0, pmax = 1;
uint32_t byte_counter, fast_recovery_times = 5, kmax = 60, kmin = 60;
std::string rate_ai, rate_hai;

bool clamp_target_rate = false, clamp_target_rate_after_timer = false, send_in_chunks = true, l2_wait_for_ack = false, l2_back_to_zero = false, l2_test_read = false;
double error_rate_per_link = 0.0;


//by derui liu


struct FLOW_STA {
	double start_time;
	int flow_size;
};

struct switch_message {//存储交换机的信息，即为了开启线程时知道其负责的是哪个队列
	uint32_t switch_id;
	uint32_t port_id;
	uint32_t queue_id;
};

struct throughput_data {
	uint64_t last_rxBytes;
	double last_time;
};

struct switch_throughput {
	uint64_t packets = 0;
	uint64_t lastpackets = 0;
};
uint32_t my_kmin = 5 * 1030;
uint32_t my_kmax = 200 * 1030;
struct ECN_data {
	uint32_t kmin = my_kmin;
	uint32_t kmax = my_kmax;
	double pmax = 0.01;
};

struct switch_train_data {
	uint64_t queue_length = 0;
	uint64_t ECN_number = 0;
	uint64_t queue_length_number = 0;
	double dequeue_tp = 0;
	uint32_t max_queue_length = 0;
	uint32_t instant_queue_length = 0;
	ECN_data ecn;
	ECN_data new_ECN;
	bool flag = false;
};
uint32_t PCnt = 31;//交换机端口个数，
uint32_t QCnt = 3;//交换机每个端口的队列个数，此处所更改，则对应broadcom-node.h中的设置也要更改,ALL_switch_collect同理
switch_train_data All_switch_collect[18][31][3];//假设18个交换机，12个tor,6个spine，每个交换机有31个端口，6个连接switch，24个连接server，每个端口有2个队列，如果更改系统的拓扑结构则需要相应更改交换机个数
switch_throughput Throughput_collect[18][31][3];//用来计算每个队列的出队吞吐量
Time Switch_Time[18][31][3];//用来记录每一个队列的时间以便求出该队列在这段时间的dequeue throughput

double my_interval = 0.001;
double load = 1.0;
uint32_t maxPacketCount_global = 2000;
FLOW_STA flow_sta[2000];
uint32_t flow_num;
uint32_t background_number;
uint32_t workload_type = 1;//对应的work_load的类型，0：datamining 1：webserach
uint32_t workload_pattern = 1;//0: permutation(possion); 1:random(possion)，0的话流的个数等于server的个数
							  //uint32_t switch_send(uint32_t switch_id, uint32_t Kmin, uint32_t sum_throughput, uint32_t switch_length);
							  //double *flow_throughput;//定义全局动态数组，用来存储所有流的吞吐量
							  //uint32_t flow_throughput_num;
uint32_t Obversation_counter = 0;
uint64_t my_switch_length = 0;
uint32_t my_switch_number = 1;
uint64_t my_switch_length_2 = 0;
uint32_t my_switch_number_2 = 1;

//Time last_packet_time = Time(Seconds(2));

//std::vector<switch_train_data> Switch_collect;//记录交换机要发出的数据
//std::vector<throughput_data> Time_collect;//记录时间和收包信息已求出时间间隔内的吞吐量
void Record_switch_length() {
	switch_mutex.lock();
	std::ofstream outfile;
	outfile.open("switch_file_17.txt", std::ios::app);
	uint32_t average = my_switch_length / my_switch_number;
	outfile << average << "\n";
	my_switch_length = 0;
	my_switch_number = 1;
	outfile.close();
	std::ofstream outfile_2;
	outfile_2.open("switch_file_19.txt", std::ios::app);
	uint32_t average_2 = my_switch_length_2 / my_switch_number_2;
	outfile_2 << average_2 << "\n";
	my_switch_length_2 = 0;
	my_switch_number_2 = 1;
	outfile_2.close();
	switch_mutex.unlock();

	Simulator::Schedule(MicroSeconds(2), Record_switch_length);
}
//calcute throughput
void
ThroughputMonitor(FlowMonitorHelper* fmhelper, Ptr<FlowMonitor> monitor)
{
	monitor->CheckForLostPackets();
	std::map<FlowId, FlowMonitor::FlowStats> flowStats = monitor->GetFlowStats();
	/* since fmhelper is a pointer, we should use it as a pointer.
	* `fmhelper->GetClassifier ()` instead of `fmhelper.GetClassifier ()`
	*/
	std::ofstream outfile;
	outfile.open("fct.txt");

	Ptr<Ipv4FlowClassifier> classifier = DynamicCast<Ipv4FlowClassifier>(fmhelper->GetClassifier());
	for (std::map<FlowId, FlowMonitor::FlowStats>::const_iterator i = flowStats.begin(); i != flowStats.end(); ++i)
	{
		//std::cout<< flowStats.end()
		/*
		* FiveTuple五元组是：(source-ip, destination-ip, protocol, source-port, destination-port)
		*/
		double th_put;
		//flow_throughput[i] = 1;
		/* 每个flow是根据包的五元组(协议，源IP/端口，目的IP/端口)来区分的 */
		Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow(i->first);
		int64_t FCT = i->second.timeLastRxPacket.GetMicroSeconds() - i->second.timeFirstTxPacket.GetMicroSeconds();
		outfile << i->second.txPackets << " " << FCT << "\n";
		//double FCT = i->second.timeLastRxPacket.GetMilliSeconds() - i->second.timeFirstTxPacket.GetMilliSeconds();
		//double FCT = i->second.timeLastRxPacket.GetSeconds() - i->second.timeFirstTxPacket.GetSeconds();

		std::cout << "Flow " << i->first << " (" << t.sourceAddress << " -> " << t.destinationAddress << ")" << FCT << " us\n";
		std::cout << "  Tx Bytes:   " << i->second.txBytes << "\n";
		std::cout << "  Rx Bytes:   " << i->second.rxBytes << "\n";
		//std::cout << Time_collect[i->first].last_rxBytes << "\n";
		th_put = i->second.rxBytes * 8.0 / (i->second.timeLastRxPacket.GetSeconds() - i->second.timeFirstTxPacket.GetSeconds()) / 1024 / 1024 / 1024.0;
		//th_put = (i->second.rxBytes - Time_collect[i->first].last_rxBytes) * 8.0 / (i->second.timeLastRxPacket.GetSeconds() - Time_collect[i->first].last_time) / 1024 / 1024 / 1024.0;
		std::cout << "  Throughput: " << th_put << " Gbps\n";
		//flow_throughput[i->first] = th_put;
		//double end_time = i->second.timeLastRxPacket.GetSeconds();
		//std::cout << "开始时间 " << Time_collect[i->first].last_time << " 结束时间" << end_time << "\n";
		//Time_collect[i->first].last_rxBytes = i->second.rxBytes;
		//Time_collect[i->first].last_time = i->second.timeLastRxPacket.GetSeconds();
	}
	outfile.close();
	//monitor->SerializeToXmlFile("lab-1.flowmon", true, true);
	/* check throughput every nSamplingPeriod second(每隔nSamplingPeriod调用依次Simulation)
	* 表示每隔nSamplingPeriod时间
	*/
	//Simulator::Schedule(MicroSeconds(20000), &ThroughputMonitor, fmhelper, monitor);
	Simulator::Schedule(MilliSeconds(2), &ThroughputMonitor, fmhelper, monitor);
}

void
SendObversationData(switch_message data) {

	thread_mutex.lock();
	//std::cout << "switch_id " << data.switch_id << " " << data.port_id << " " << data.queue_id << "\n";
	void *context = zmq_ctx_new();
	void *responder = zmq_socket(context, ZMQ_REQ);
	std::string server_a1 = "tcp://localhost:";//0，1，2；0，2，2；
	uint32_t number = data.switch_id*(PCnt - 1) + (data.port_id);//只监听一个队列
	uint32_t p = 5554 + number;
	std::string server_address = server_a1 + std::to_string(p);

	const char *switch_data = const_cast<char *>(server_address.c_str());
	//std::cout << "adderss  " << switch_data << "\n";
	int rc = zmq_connect(responder, switch_data);
	assert(rc == 0);
	thread_mutex.unlock();
	//send_train_message(responder, data);



	//if (zmq_size == -1) {
	//	std::cout << p<<" receive failed\n";
	//}
	//std::cout << buffer << "\n";

	uint64_t packets = Throughput_collect[data.switch_id][data.port_id][data.queue_id].packets;

	mutex.lock();
	Throughput_collect[data.switch_id][data.port_id][data.queue_id].packets = 0;
	//std::cout << packets << "\n";
	mutex.unlock();

	Time time = Simulator::Now();

	//求链路带宽
	//std::cout << "time.GetMilliSeconds() " << time.GetSeconds() << "last " << Switch_Time[data.switch_id][data.port_id][data.queue_id].GetSeconds() << "\n";
	double difference_time = time.GetMicroSeconds() - Switch_Time[data.switch_id][data.port_id][data.queue_id].GetMicroSeconds();
	//std::cout << "difference time " << difference_time << "\n";
	Switch_Time[data.switch_id][data.port_id][data.queue_id] = time;
	double throughput = packets *8.0 * 1020 * 1000 * 1000 / difference_time / 1024 / 1024 / 1024.0;
	//std::cout << "throughput " << throughput << "\n";
	All_switch_collect[data.switch_id][data.port_id][data.queue_id].dequeue_tp = throughput;

	//All_switch_collect[data.switch_id][data.port_id][data.queue_id].lastpackets = Throughput_collect[data.switch_id][data.port_id][data.queue_id].packets;
	ECN_mutex.lock();//求ECN速率
	uint64_t ECN_ratio = 1000 * 1000 * All_switch_collect[data.switch_id][data.port_id][data.queue_id].ECN_number / difference_time;
	All_switch_collect[data.switch_id][data.port_id][data.queue_id].ECN_number = 0;
	ECN_mutex.unlock();
	if (difference_time == 0) {
		ECN_ratio = 0;
		throughput = 0;

	}
	//std::cout << "ECN_ratio " << ECN_ratio << " \n";
	//::cout << Switch_collect[switch_id].last_ecn_number << "   " << Switch_collect[switch_id].ECN_number << "\n";
	//All_switch_collect[data.switch_id][data.port_id][data.queue_id].last_ecn_number = All_switch_collect[data.switch_id][data.port_id][data.queue_id].ECN_number;

	uint64_t queue_length_number = All_switch_collect[data.switch_id][data.port_id][data.queue_id].queue_length_number;
	uint32_t queue_length = 0;
	if (queue_length_number != 0)
	{
		queue_length_mutex.lock();//求平均队列长度
		queue_length = All_switch_collect[data.switch_id][data.port_id][data.queue_id].queue_length / queue_length_number;
		All_switch_collect[data.switch_id][data.port_id][data.queue_id].queue_length = 0;
		All_switch_collect[data.switch_id][data.port_id][data.queue_id].queue_length_number = 0;
		queue_length_mutex.unlock();
	}
	uint32_t max_queuelength = All_switch_collect[data.switch_id][data.port_id][data.queue_id].max_queue_length;
	All_switch_collect[data.switch_id][data.port_id][data.queue_id].max_queue_length = 0;
	std::string queue_data = std::to_string(data.switch_id) + " " + std::to_string(data.port_id) + " 2";

	std::string switch_predata = queue_data + "," + std::to_string(queue_length) + ","
		+ std::to_string(ECN_ratio) + "," + std::to_string(throughput);
	//std::cout << " std::to_string(ECN_ratio)    " << std::to_string(ECN_ratio) << "\n";
	//在此处加入最大max_queue_length
	switch_predata = switch_predata + "," + std::to_string(max_queuelength);
	uint32_t instant_queue_l = All_switch_collect[data.switch_id][data.port_id][data.queue_id].instant_queue_length;
	switch_predata = switch_predata + "," + std::to_string(instant_queue_l);

	switch_predata = switch_predata + "," + std::to_string(All_switch_collect[data.switch_id][data.port_id][data.queue_id].ecn.kmin / 1030) + "," + std::to_string(All_switch_collect[data.switch_id][data.port_id][data.queue_id].ecn.kmax / 1030)
		+ "," + std::to_string(All_switch_collect[data.switch_id][data.port_id][data.queue_id].ecn.pmax);


	int interval = 400;
	interval = 400 * (1 - 0.6*(queue_length + instant_queue_l) / max_queuelength);
	if (interval < 0) interval = 400;
	switch_predata = switch_predata + "," + std::to_string(interval);
	//std::cout << "switch_predata " << switch_predata << "\n";
	const char *switch_data1 = const_cast<char *>(switch_predata.c_str());
	//sleep(1);          //  Do some 'work'
	//std::cout << switch_data1 << "\n";
	zmq_send(responder, switch_data1, switch_predata.length(), 0);
	//if (zmq_size == -1) {
	//	std::cout << p << " send failed\n";
	//新添的代码
	Sleep(1.0);
	char buffer[13];
	zmq_recv(responder, buffer, 11, 0);
	std::string receive_kmin = strtok(buffer, ",");
	ECN_flag_mutex.lock();
	if (receive_kmin[0] != '0') {
		ECN_data new_ecn_data;
		new_ecn_data.kmin = std::stoi(receive_kmin);
		std::string receive_kmax = strtok(NULL, ",");
		new_ecn_data.kmax = std::stoi(receive_kmax);
		std::string receive_pmax = strtok(NULL, ",");
		new_ecn_data.pmax = stod(receive_pmax);
		//std::cout << "pmax   " << new_ecn_data.pmax << "\n";
		All_switch_collect[data.switch_id][data.port_id][data.queue_id].new_ECN = new_ecn_data;
		All_switch_collect[data.switch_id][data.port_id][data.queue_id].flag = true;
	}
	ECN_flag_mutex.unlock();

	//}
	zmq_close(responder);
	zmq_ctx_destroy(context);

	Simulator::Schedule(MicroSeconds(400), &SendObversationData, data);


}

void
Run_thread() {
	//线程个数
	//boost::thread t[4][16][2];
	//一个线程监听交换机上的一个队列，只需要监听12个交换机即可
	for (uint32_t i = 0; i < 3; i++) {//交换机个数
		for (uint32_t j = 1; j < PCnt; j++) {//端口个数
			for (uint32_t k = 2; k < QCnt; k++) {//队列个数，只监听队列2
				switch_message data;
				data.switch_id = i;
				data.port_id = j;
				data.queue_id = k;
				Time time = Time(MilliSeconds(2000));
				Switch_Time[i][j][k] = time;
				boost::thread(SendObversationData, data).detach();
			}
		}
	}
}

//生成指数分布的随机数（对应的流的到达时间，则流的分布是泊松分布）
long double randomExponential(long double lamda)
{
	double pV = 0.0;

	while (true)
	{
		pV = (double)rand() / (double)RAND_MAX;
		if (pV != 1)
			break;
	}
	pV = (-1.0 / lamda)*log(1 - pV);
	return pV;
}
//flow attributes
uint32_t min_flow_size = 6;
uint32_t max_flow_size = 20000;
uint32_t av_flow_size = 1138;
int64_t random_seed = 2;

Ptr<RandomVariableStream>
GetDataMiningStream(void)
{
	Ptr<EmpiricalRandomVariable> stream = CreateObject<EmpiricalRandomVariable>();
	stream->SetStream(random_seed);
	stream->CDF(1, 0.0);
	stream->CDF(1, 0.5);
	stream->CDF(2, 0.6);
	stream->CDF(3, 0.7);
	stream->CDF(7, 0.8);
	stream->CDF(267, 0.9);
	stream->CDF(2107, 0.95);
	stream->CDF(66667, 0.99);
	stream->CDF(666667, 1.0);
	min_flow_size = 1;
	max_flow_size = 666667;
	av_flow_size = 5116;
	return stream;
}

Ptr<RandomVariableStream>
GetWebSearchStream(void)
{
	Ptr<EmpiricalRandomVariable> stream = CreateObject<EmpiricalRandomVariable>();
	stream->SetStream(random_seed);
	stream->CDF(6, 0.0);
	stream->CDF(6, 0.15);
	stream->CDF(13, 0.2);
	stream->CDF(19, 0.3);
	stream->CDF(33, 0.4);
	stream->CDF(53, 0.53);
	stream->CDF(133, 0.6);
	stream->CDF(667, 0.7);
	stream->CDF(1333, 0.8);
	stream->CDF(3333, 0.9);
	stream->CDF(6667, 0.97);
	stream->CDF(20000, 1.0);
	min_flow_size = 6;
	max_flow_size = 20000;
	av_flow_size = 1138;
	return stream;
}
Ptr<RandomVariableStream>
GetHadoopStream(void)
{
	Ptr<EmpiricalRandomVariable> stream = CreateObject<EmpiricalRandomVariable>();
	stream->SetStream(random_seed);
	stream->CDF(1, 0.0);
	stream->CDF(1, 0.1);
	stream->CDF(333, 0.2);
	stream->CDF(666, 0.3);
	stream->CDF(1000, 0.4);
	stream->CDF(3000, 0.6);
	stream->CDF(5000, 0.8);
	stream->CDF(1000000, 0.95);
	stream->CDF(6666667, 1.0);
	min_flow_size = 1;
	max_flow_size = 6666667;
	av_flow_size = 268392;
	return stream;
}

int main(int argc, char *argv[])
{
	clock_t begint, endt;
	begint = clock();
#ifndef PGO_TRAINING
	if (argc > 1)
#else
	if (true)
#endif
	{
		//Read the configuration file
		std::ifstream conf;
#ifndef PGO_TRAINING
		conf.open(argv[1]);
#else
		conf.open(PATH_TO_PGO_CONFIG);
#endif
		while (!conf.eof())
		{
			std::string key;
			conf >> key;

			//std::cout << conf.cur << "\n";

			if (key.compare("ENABLE_QCN") == 0)
			{
				uint32_t v;
				conf >> v;
				enable_qcn = v;
				if (enable_qcn)
					std::cout << "ENABLE_QCN\t\t\t" << "Yes" << "\n";
				else
					std::cout << "ENABLE_QCN\t\t\t" << "No" << "\n";
			}
			else if (key.compare("USE_DYNAMIC_PFC_THRESHOLD") == 0)
			{
				uint32_t v;
				conf >> v;
				use_dynamic_pfc_threshold = v;
				if (use_dynamic_pfc_threshold)
					std::cout << "USE_DYNAMIC_PFC_THRESHOLD\t" << "Yes" << "\n";
				else
					std::cout << "USE_DYNAMIC_PFC_THRESHOLD\t" << "No" << "\n";
			}
			else if (key.compare("CLAMP_TARGET_RATE") == 0)
			{
				uint32_t v;
				conf >> v;
				clamp_target_rate = v;
				if (clamp_target_rate)
					std::cout << "CLAMP_TARGET_RATE\t\t" << "Yes" << "\n";
				else
					std::cout << "CLAMP_TARGET_RATE\t\t" << "No" << "\n";
			}
			else if (key.compare("CLAMP_TARGET_RATE_AFTER_TIMER") == 0)
			{
				uint32_t v;
				conf >> v;
				clamp_target_rate_after_timer = v;
				if (clamp_target_rate_after_timer)
					std::cout << "CLAMP_TARGET_RATE_AFTER_TIMER\t" << "Yes" << "\n";
				else
					std::cout << "CLAMP_TARGET_RATE_AFTER_TIMER\t" << "No" << "\n";
			}
			else if (key.compare("PACKET_LEVEL_ECMP") == 0)
			{
				uint32_t v;
				conf >> v;
				packet_level_ecmp = v;
				if (packet_level_ecmp)
					std::cout << "PACKET_LEVEL_ECMP\t\t" << "Yes" << "\n";
				else
					std::cout << "PACKET_LEVEL_ECMP\t\t" << "No" << "\n";
			}
			else if (key.compare("FLOW_LEVEL_ECMP") == 0)
			{
				uint32_t v;
				conf >> v;
				flow_level_ecmp = v;
				if (flow_level_ecmp)
					std::cout << "FLOW_LEVEL_ECMP\t\t\t" << "Yes" << "\n";
				else
					std::cout << "FLOW_LEVEL_ECMP\t\t\t" << "No" << "\n";
			}
			else if (key.compare("PAUSE_TIME") == 0)
			{
				double v;
				conf >> v;
				pause_time = v;
				std::cout << "PAUSE_TIME\t\t\t" << pause_time << "\n";
			}
			else if (key.compare("DATA_RATE") == 0)
			{
				std::string v;
				conf >> v;
				data_rate = v;
				std::cout << "DATA_RATE\t\t\t" << data_rate << "\n";
			}
			else if (key.compare("LINK_DELAY") == 0)
			{
				std::string v;
				conf >> v;
				link_delay = v;
				std::cout << "LINK_DELAY\t\t\t" << link_delay << "\n";
			}
			else if (key.compare("PACKET_PAYLOAD_SIZE") == 0)
			{
				uint32_t v;
				conf >> v;
				packet_payload_size = v;
				std::cout << "PACKET_PAYLOAD_SIZE\t\t" << packet_payload_size << "\n";
			}
			else if (key.compare("L2_CHUNK_SIZE") == 0)
			{
				uint32_t v;
				conf >> v;
				l2_chunk_size = v;
				std::cout << "L2_CHUNK_SIZE\t\t\t" << l2_chunk_size << "\n";
			}
			else if (key.compare("L2_ACK_INTERVAL") == 0)
			{
				uint32_t v;
				conf >> v;
				l2_ack_interval = v;
				std::cout << "L2_ACK_INTERVAL\t\t\t" << l2_ack_interval << "\n";
			}
			else if (key.compare("L2_WAIT_FOR_ACK") == 0)
			{
				uint32_t v;
				conf >> v;
				l2_wait_for_ack = v;
				if (l2_wait_for_ack)
					std::cout << "L2_WAIT_FOR_ACK\t\t\t" << "Yes" << "\n";
				else
					std::cout << "L2_WAIT_FOR_ACK\t\t\t" << "No" << "\n";
			}
			else if (key.compare("L2_BACK_TO_ZERO") == 0)
			{
				uint32_t v;
				conf >> v;
				l2_back_to_zero = v;
				if (l2_back_to_zero)
					std::cout << "L2_BACK_TO_ZERO\t\t\t" << "Yes" << "\n";
				else
					std::cout << "L2_BACK_TO_ZERO\t\t\t" << "No" << "\n";
			}
			else if (key.compare("L2_TEST_READ") == 0)
			{
				uint32_t v;
				conf >> v;
				l2_test_read = v;
				if (l2_test_read)
					std::cout << "L2_TEST_READ\t\t\t" << "Yes" << "\n";
				else
					std::cout << "L2_TEST_READ\t\t\t" << "No" << "\n";
			}
			else if (key.compare("TOPOLOGY_FILE") == 0)
			{
				std::string v;
				conf >> v;
				topology_file = v;
				std::cout << "TOPOLOGY_FILE\t\t\t" << topology_file << "\n";
			}
			else if (key.compare("FLOW_FILE") == 0)
			{
				std::string v;
				conf >> v;
				flow_file = v;
				std::cout << "FLOW_FILE\t\t\t" << flow_file << "\n";
			}
			else if (key.compare("TCP_FLOW_FILE") == 0)
			{
				std::string v;
				conf >> v;
				tcp_flow_file = v;
				std::cout << "TCP_FLOW_FILE\t\t\t" << tcp_flow_file << "\n";
			}
			else if (key.compare("TRACE_FILE") == 0)
			{
				std::string v;
				conf >> v;
				trace_file = v;
				std::cout << "TRACE_FILE\t\t\t" << trace_file << "\n";
			}
			else if (key.compare("TRACE_OUTPUT_FILE") == 0)
			{
				std::string v;
				conf >> v;
				trace_output_file = v;
				if (argc > 2)
				{
					trace_output_file = trace_output_file + std::string(argv[2]);
				}
				std::cout << "TRACE_OUTPUT_FILE\t\t" << trace_output_file << "\n";
			}
			else if (key.compare("my_flow_file") == 0)
			{
				std::string v;
				conf >> v;
				myflow_file = v;
				if (argc > 2)
				{
					trace_output_file = trace_output_file + std::string(argv[2]);
				}
				//std::cout << "TRACE_OUTPUT_FILE\t\t" << trace_output_file << "\n";
			}
			else if (key.compare("APP_START_TIME") == 0)
			{
				double v;
				conf >> v;
				app_start_time = v;
				std::cout << "SINK_START_TIME\t\t\t" << app_start_time << "\n";
			}
			else if (key.compare("APP_STOP_TIME") == 0)
			{
				double v;
				conf >> v;
				app_stop_time = v;
				std::cout << "SINK_STOP_TIME\t\t\t" << app_stop_time << "\n";
			}
			else if (key.compare("SIMULATOR_STOP_TIME") == 0)
			{
				double v;
				conf >> v;
				simulator_stop_time = v;
				std::cout << "SIMULATOR_STOP_TIME\t\t" << simulator_stop_time << "\n";
			}
			else if (key.compare("CNP_INTERVAL") == 0)
			{
				double v;
				conf >> v;
				cnp_interval = v;
				std::cout << "CNP_INTERVAL\t\t\t" << cnp_interval << "\n";
			}
			else if (key.compare("ALPHA_RESUME_INTERVAL") == 0)
			{
				double v;
				conf >> v;
				alpha_resume_interval = v;
				std::cout << "ALPHA_RESUME_INTERVAL\t\t" << alpha_resume_interval << "\n";
			}
			else if (key.compare("RP_TIMER") == 0)
			{
				double v;
				conf >> v;
				rp_timer = v;
				std::cout << "RP_TIMER\t\t\t" << rp_timer << "\n";
			}
			else if (key.compare("BYTE_COUNTER") == 0)
			{
				uint32_t v;
				conf >> v;
				byte_counter = v;
				std::cout << "BYTE_COUNTER\t\t\t" << byte_counter << "\n";
			}
			else if (key.compare("KMAX") == 0)
			{
				uint32_t v;
				conf >> v;
				kmax = v;
				std::cout << "KMAX\t\t\t\t" << kmax << "\n";
			}
			else if (key.compare("KMIN") == 0)
			{
				uint32_t v;
				conf >> v;
				kmin = v;
				std::cout << "KMIN\t\t\t\t" << kmin << "\n";
			}
			else if (key.compare("PMAX") == 0)
			{
				double v;
				conf >> v;
				pmax = v;
				std::cout << "PMAX\t\t\t\t" << pmax << "\n";
			}
			else if (key.compare("DCTCP_GAIN") == 0)
			{
				double v;
				conf >> v;
				dctcp_gain = v;
				std::cout << "DCTCP_GAIN\t\t\t" << dctcp_gain << "\n";
			}
			else if (key.compare("FAST_RECOVERY_TIMES") == 0)
			{
				uint32_t v;
				conf >> v;
				fast_recovery_times = v;
				std::cout << "FAST_RECOVERY_TIMES\t\t" << fast_recovery_times << "\n";
			}
			else if (key.compare("RATE_AI") == 0)
			{
				std::string v;
				conf >> v;
				rate_ai = v;
				std::cout << "RATE_AI\t\t\t\t" << rate_ai << "\n";
			}
			else if (key.compare("RATE_HAI") == 0)
			{
				std::string v;
				conf >> v;
				rate_hai = v;
				std::cout << "RATE_HAI\t\t\t" << rate_hai << "\n";
			}
			else if (key.compare("NP_SAMPLING_INTERVAL") == 0)
			{
				double v;
				conf >> v;
				np_sampling_interval = v;
				std::cout << "NP_SAMPLING_INTERVAL\t\t" << np_sampling_interval << "\n";
			}
			else if (key.compare("SEND_IN_CHUNKS") == 0)
			{
				uint32_t v;
				conf >> v;
				send_in_chunks = v;
				if (send_in_chunks)
				{
					std::cout << "SEND_IN_CHUNKS\t\t\t" << "Yes" << "\n";
					std::cout << "WARNING: deprecated and not tested. Please consider using L2_WAIT_FOR_ACK";
				}
				else
					std::cout << "SEND_IN_CHUNKS\t\t\t" << "No" << "\n";
			}
			else if (key.compare("ERROR_RATE_PER_LINK") == 0)
			{
				double v;
				conf >> v;
				error_rate_per_link = v;
				std::cout << "ERROR_RATE_PER_LINK\t\t" << error_rate_per_link << "\n";
			}
			else if (key.compare("workload_type") == 0)
			{
				uint32_t v;
				conf >> v;
				workload_type = v;
				std::cout << "workload_type\t\t\t" << workload_type << "\n";
			}
			else if (key.compare("workload_pattern") == 0)
			{
				uint32_t v;
				conf >> v;
				workload_pattern = v;
				std::cout << "workload_pattern\t\t" << workload_type << "\n";
			}
			else if (key.compare("background_number") == 0) {
				uint32_t v;
				conf >> v;
				background_number = v;
				std::cout << "background_number\t\t" << background_number << std::endl;
			}
			else if (key.compare("load") == 0) {
				double v;
				conf >> v;
				load = v;
				std::cout << "load\t\t\t\t" << load << std::endl;
			}
			else if (key.compare("maxPacketCount_global") == 0) {
				uint32_t v;
				conf >> v;
				maxPacketCount_global = v;
				std::cout << "maxPacketCount_global\t\t" << maxPacketCount_global << std::endl;
			}
			else if (key.compare("my_interval") == 0) {
				double v;
				conf >> v;
				my_interval = v;
				std::cout << "my_interval\t\t\t" << my_interval << std::endl;
			}
			else if (key.compare("average_flow_size") == 0) {
				double v;
				conf >> v;
				average_flow_size = v;
				std::cout << "average_flow_size\t\t" << average_flow_size << std::endl;
			}

			fflush(stdout);
		}
		conf.close();
	}
	else
	{
		std::cout << "Error: require a config file\n";
		fflush(stdout);
		return 1;
	}


	bool dynamicth = use_dynamic_pfc_threshold;

	NS_ASSERT(packet_level_ecmp + flow_level_ecmp < 2); //packet level ecmp and flow level ecmp are exclusive
	Config::SetDefault("ns3::Ipv4GlobalRouting::RandomEcmpRouting", BooleanValue(packet_level_ecmp));
	Config::SetDefault("ns3::Ipv4GlobalRouting::FlowEcmpRouting", BooleanValue(flow_level_ecmp));
	Config::SetDefault("ns3::QbbNetDevice::PauseTime", UintegerValue(pause_time));
	Config::SetDefault("ns3::QbbNetDevice::QcnEnabled", BooleanValue(enable_qcn));
	Config::SetDefault("ns3::QbbNetDevice::DynamicThreshold", BooleanValue(dynamicth));
	Config::SetDefault("ns3::QbbNetDevice::ClampTargetRate", BooleanValue(clamp_target_rate));
	Config::SetDefault("ns3::QbbNetDevice::ClampTargetRateAfterTimeInc", BooleanValue(clamp_target_rate_after_timer));
	Config::SetDefault("ns3::QbbNetDevice::CNPInterval", DoubleValue(cnp_interval));
	Config::SetDefault("ns3::QbbNetDevice::NPSamplingInterval", DoubleValue(np_sampling_interval));
	Config::SetDefault("ns3::QbbNetDevice::AlphaResumInterval", DoubleValue(alpha_resume_interval));
	Config::SetDefault("ns3::QbbNetDevice::RPTimer", DoubleValue(rp_timer));
	Config::SetDefault("ns3::QbbNetDevice::ByteCounter", UintegerValue(byte_counter));
	Config::SetDefault("ns3::QbbNetDevice::FastRecoveryTimes", UintegerValue(fast_recovery_times));
	Config::SetDefault("ns3::QbbNetDevice::DCTCPGain", DoubleValue(dctcp_gain));
	Config::SetDefault("ns3::QbbNetDevice::RateAI", DataRateValue(DataRate(rate_ai)));
	Config::SetDefault("ns3::QbbNetDevice::RateHAI", DataRateValue(DataRate(rate_hai)));
	Config::SetDefault("ns3::QbbNetDevice::L2BackToZero", BooleanValue(l2_back_to_zero));
	Config::SetDefault("ns3::QbbNetDevice::L2TestRead", BooleanValue(l2_test_read));
	Config::SetDefault("ns3::QbbNetDevice::L2ChunkSize", UintegerValue(l2_chunk_size));
	Config::SetDefault("ns3::QbbNetDevice::L2AckInterval", UintegerValue(l2_ack_interval));
	Config::SetDefault("ns3::QbbNetDevice::L2WaitForAck", BooleanValue(l2_wait_for_ack));

	SeedManager::SetSeed(time(NULL));

	std::ifstream topof, flowf, tracef, tcpflowf, infile;
	topof.open(topology_file.c_str());
	flowf.open(flow_file.c_str());
	tracef.open(trace_file.c_str());
	tcpflowf.open(tcp_flow_file.c_str());
	infile.open(myflow_file.c_str());
	if (!infile) {
		std::cout << "11111111111\n";
	}

	uint32_t node_num, switch_num, trace_num, tcp_flow_num;
	uint32_t server_num;
	uint32_t server_per_rack;//每个交换机连多少个server
							 //uint32_t node_num, switch_num, link_num, flow_num, trace_num, tcp_flow_num;
							 //topof >> node_num >> switch_num >> link_num;
	topof >> switch_num >> server_per_rack >> flow_num;
	//flowf >> flow_num;
	tracef >> trace_num;
	tcpflowf >> tcp_flow_num;

	server_num = 12 * server_per_rack;//12个tor交换机
	node_num = server_num + switch_num;

	std::cout << "node " << node_num << " switch " << switch_num << " server " << server_num << std::endl;
	//程序设定topology和flow，不从配置文件读入

	NodeContainer n;
	n.Create(node_num);

	//std::cout << std::fixed << std::setprecision(2);//全文保留2位小数
	for (uint32_t i = 0; i < switch_num; i++)
	{
		//switch_train_data train_data;
		//Switch_collect.push_back(train_data);
		//switch_throughput data;
		//Throughput_collect.push_back(data);

		uint32_t sid;
		sid = i;
		/*得到Node指针 这个SetNodeType函数是作者自己定义的，第一个参数为1表示创建的节点类型为BroadcomNode*/
		n.Get(sid)->SetNodeType(1, dynamicth); //broadcom switch
		n.Get(sid)->m_broadcom->SetMarkingThreshold(kmin, kmax, pmax);
		//t[i] = boost::thread(SendObversationData, i);
		//t[i].join();
	}


	//SendObversationData(switch_num);

	NS_LOG_INFO("Create nodes.");

	InternetStackHelper internet;
	internet.Install(n);

	NS_LOG_INFO("Create channels.");

	//
	// Explicitly create the channels required by the topology.
	//
	/*rem决定哪个包会出错，出错的分布就是uv~平均分布*/
	Ptr<RateErrorModel> rem = CreateObject<RateErrorModel>();
	Ptr<UniformRandomVariable> uv = CreateObject<UniformRandomVariable>();
	rem->SetRandomVariable(uv);
	uv->SetStream(50);
	rem->SetAttribute("ErrorRate", DoubleValue(error_rate_per_link));
	rem->SetAttribute("ErrorUnit", StringValue("ERROR_UNIT_PACKET"));

	QbbHelper qbb;
	Ipv4AddressHelper ipv4;

	int index = 0;//用于生成ip

				  //switch 之间的link,交换机中间的结构，clos结构
	for (int i = 0; i < 12; i++) {//12个tor交换机
								  //std::cout << "test " << std::endl;
		for (int j = 12; j < 18; j++) {//6个spine交换机

			uint32_t src, dst;
			std::string data_rate, link_delay;
			double error_rate;
			src = i;
			dst = j;
			data_rate = "100Gbps";
			link_delay = "0.001ms";
			error_rate = 0;

			qbb.SetDeviceAttribute("DataRate", StringValue(data_rate));
			qbb.SetChannelAttribute("Delay", StringValue(link_delay));

			if (error_rate > 0)
			{
				Ptr<RateErrorModel> rem = CreateObject<RateErrorModel>();
				Ptr<UniformRandomVariable> uv = CreateObject<UniformRandomVariable>();
				rem->SetRandomVariable(uv);
				uv->SetStream(50);
				rem->SetAttribute("ErrorRate", DoubleValue(error_rate));
				rem->SetAttribute("ErrorUnit", StringValue("ERROR_UNIT_PACKET"));
				qbb.SetDeviceAttribute("ReceiveErrorModel", PointerValue(rem));
			}
			else
			{
				qbb.SetDeviceAttribute("ReceiveErrorModel", PointerValue(rem));
			}

			NetDeviceContainer d = qbb.Install(n.Get(src), n.Get(dst));/*src和dst都是用node的序号来表示的*/

																	   //std::cout << "test index " << index << std::endl;

			char ipstring[16];
			/*生成每个node的IP address*/
			sprintf(ipstring, "10.%d.%d.0", index / 254 + 1, index % 254 + 1);
			ipv4.SetBase(ipstring, "255.255.255.0");
			ipv4.Assign(d);
			index++;
		}
	}

	//switch和server之间的链路
	for (int i = 0; i < server_num; i++) {//每个tor连接24个server
		uint32_t src, dst;
		std::string data_rate, link_delay;
		double error_rate;

		src = i + switch_num;
		dst = i / server_per_rack; //每个switch连接 server_per_rack 个server
		data_rate = "25Gbps";
		link_delay = "0.001ms";
		error_rate = 0;

		qbb.SetDeviceAttribute("DataRate", StringValue(data_rate));
		qbb.SetChannelAttribute("Delay", StringValue(link_delay));

		if (error_rate > 0)
		{
			Ptr<RateErrorModel> rem = CreateObject<RateErrorModel>();
			Ptr<UniformRandomVariable> uv = CreateObject<UniformRandomVariable>();
			rem->SetRandomVariable(uv);
			uv->SetStream(50);

			rem->SetAttribute("ErrorRate", DoubleValue(error_rate));
			rem->SetAttribute("ErrorUnit", StringValue("ERROR_UNIT_PACKET"));
			qbb.SetDeviceAttribute("ReceiveErrorModel", PointerValue(rem));/*这是啥属性呢~不懂*/
		}
		else
		{
			qbb.SetDeviceAttribute("ReceiveErrorModel", PointerValue(rem));
		}

		NetDeviceContainer d = qbb.Install(n.Get(src), n.Get(dst));/*src和dst都是用node的序号来表示的*/

		char ipstring[16];
		/*生成每个node的IP address*/
		sprintf(ipstring, "10.%d.%d.0", index / 254 + 1, index % 254 + 1);
		ipv4.SetBase(ipstring, "255.255.255.0");
		ipv4.Assign(d);
		index++;
	}

	NodeContainer trace_nodes;
	for (uint32_t i = 0; i < trace_num; i++)
	{
		uint32_t nid;
		tracef >> nid;
		trace_nodes = NodeContainer(trace_nodes, n.Get(nid));
	}
	AsciiTraceHelper ascii;
	qbb.EnableAscii(ascii.CreateFileStream(trace_output_file), trace_nodes);

	Ipv4GlobalRoutingHelper::PopulateRoutingTables();

	NS_LOG_INFO("Create Applications.");

	uint32_t packetSize = packet_payload_size;
	Time interPacketInterval = Seconds(0.0000005 / 2);


	Ptr<RandomVariableStream> flowSizeStream;
	switch (workload_type) {
	case 0:
		flowSizeStream = GetDataMiningStream();
		break;
	case 1:
		flowSizeStream = GetWebSearchStream();
		break;
	case 2:
		flowSizeStream = GetHadoopStream();
		break;
	default:
		break;
	}

	const double link_bw = 25 * 1e9; //25G
	av_flow_size = average_flow_size;//对应websearch时的av_flow_size
									 //av_flow_size = 5116;
									 //av_flow_size = 268392;//对应hadoop时的av_flow_size
									 //av_flow_size = 2;
	double a = av_flow_size * 1000 * 8;
	std::cout << load << " " << link_bw << " " << server_num << "\n";
	double b = load * link_bw * server_num;
	std::cout << a << " " << b << "\n";
	double avArrivalInterval = a / b;//1代表链路负载，此时可以跑到100%
									 //std::cout << av_flow_size * 1000 * 8 << " " << link_bw*server_num << "\n";
	std::cout << "arrive interval " << avArrivalInterval << std::endl;
	//lamda = 指数分布的期望 = 流的到达时间的时间间隔
	double lamda = 1 / avArrivalInterval;
	if (lamda > 0) std::cout << "lamda " << lamda << " \n";
	double start_time, stop_time;
	start_time = 2.000000000;
	stop_time = 4.0;
	//workload_pattern: 0: permutation(possion); 1:random(possion) flow

	if (workload_pattern == 0) {

		flow_num = server_num; //permutation的flow_num不需要读入，flow的数量就等于server的数量

		for (uint32_t i = 0; i < server_num; i++) {

			int src, dst, pg, maxPacketCount, port;

			while (used_port[port = int(UniformVariable(0, 1).GetValue() * 40000)])/*如果port使用过，就重新生成port*/
				continue;
			used_port[port] = true;
			pg = 2;//优先级

			src = i + switch_num; //0+12
			dst = (i + server_per_rack) % server_num + switch_num; //(0+4)%8 + 12

																   //std::cout << "flow " << i + 1 << " src " << src << " dst " << dst << std::endl;

																   //arrival interval
																   //std::cout << "lamda" << lamda << "\n";
			double interval = randomExponential(lamda);
			//double my_start_time = start_time + interval;
			start_time += interval;  //start_time=2+interval1+interval2+...
									 //std::cout << "interval " << interval << " " << start_time << "\n";
									 //std::cout << "interval " << interval << " start " << start_time << std::endl;

									 //flow pkt number
			maxPacketCount = flowSizeStream->GetInteger();

			//为flow_sta结构体赋值 用于统计 结构体数组下标代表了flowid
			flow_sta[i + 1].flow_size = maxPacketCount;
			flow_sta[i + 1].start_time = start_time;

			//此处看的不是太懂
			//if (global_flowcell_mode || part_flowcell_mode || change_flow)
			//	maxPacketCount_global = int(maxPacketCount*k_m);

			std::cout << "flow " << i + 1 << " src " << src << " dst " << dst <<
				" start " << start_time << " size " << maxPacketCount << "\n ";
			//outfile << "size " << maxPacketCount << " size km " << maxPacketCount_global << ",";

			NS_ASSERT(n.Get(src)->GetNodeType() == 0 && n.Get(dst)->GetNodeType() == 0);
			Ptr<Ipv4> ipv4 = n.Get(dst)->GetObject<Ipv4>();
			Ipv4Address serverAddress = ipv4->GetAddress(1, 0).GetLocal(); //GetAddress(0,0) is the loopback 127.0.0.1

			if (send_in_chunks)/*是否切片*/
			{
				UdpEchoServerHelper server0(port, pg); //Add Priority /*pg是 priority group*/
				ApplicationContainer apps0s = server0.Install(n.Get(dst));
				apps0s.Start(Seconds(app_start_time));
				apps0s.Stop(Seconds(app_stop_time));
				UdpEchoClientHelper client0(serverAddress, port, pg); //Add Priority
				client0.SetAttribute("MaxPackets", UintegerValue(maxPacketCount));
				client0.SetAttribute("Interval", TimeValue(interPacketInterval));
				client0.SetAttribute("PacketSize", UintegerValue(packetSize));
				ApplicationContainer apps0c = client0.Install(n.Get(src));
				apps0c.Start(Seconds(start_time));
				apps0c.Stop(Seconds(stop_time));
			}
			else
			{
				UdpServerHelper server0(port);
				ApplicationContainer apps0s = server0.Install(n.Get(dst));
				apps0s.Start(Seconds(app_start_time));
				apps0s.Stop(Seconds(app_stop_time));
				UdpClientHelper client0(serverAddress, port, pg); //Add Priority

																  //std::cout << "pkt num " << maxPacketCount_global << std::endl;
				client0.SetAttribute("MaxPackets", UintegerValue(maxPacketCount_global));
				client0.SetAttribute("Interval", TimeValue(interPacketInterval));
				client0.SetAttribute("PacketSize", UintegerValue(packetSize));

				//client0.SetAttribute("ChunkSize", UintegerValue(l2_chunk_size));//added by lkx
				//client0.SetAttribute("SendTwice", UintegerValue(0));//added by lkx
				//client0.SetAttribute("FlowId", UintegerValue(i + 1));

				ApplicationContainer apps0c = client0.Install(n.Get(src));
				apps0c.Start(Seconds(start_time));
				apps0c.Stop(Seconds(stop_time));
			}
		}
		//outfile << std::endl;
	}
	else if (workload_pattern == 1) {// random
									 //bool flow_exit[100][100] = { false };//100: max-server-number
									 //std::ofstream outfile;
									 //outfile.open("my_flow.txt");

									 //infile.open("my_flow.txt");

		for (uint32_t i = 0; i < flow_num; i++) {

			int src, dst, pg, maxPacketCount, port;

			while (used_port[port = int(UniformVariable(0, 1).GetValue() * 40000)])/*如果port使用过，就重新生成port直到没有用过*/
				continue;
			used_port[port] = true;
			pg = 2;
			//maxPacketCount = 200; //200KB

			src = int(UniformVariable(0, 1).GetValue() * server_num);
			dst = int(UniformVariable(0, 1).GetValue() * server_num);

			while (src / server_per_rack == dst / server_per_rack)
			{
				src = int(UniformVariable(0, 1).GetValue() * server_num);
				dst = int(UniformVariable(0, 1).GetValue() * server_num);
			}
			//flow_exit[src][dst] = true;

			//之前的偏移量是0，要加上switch_num的偏移量才是src和dst的实际下标
			src += switch_num;
			dst += switch_num;

			//arrival interval
			//std::cout << "lamda" << lamda << "\n";
			double interval = randomExponential(lamda);
			//std::cout << "interval " << interval << "\n";
			//double my_start_time = start_time + interval;
			start_time += interval;  //start_time=2+interval1+interval2+...
									 //start_time += my_interval;
									 //std::cout << "interval " << interval << " start " << start_time << std::endl;
									 //flow pkt number

			maxPacketCount = flowSizeStream->GetInteger();

			//为flow_sta结构体赋值 用于统计 结构体数组下标代表了flowid
			//flow_sta[i + 1].flow_size = maxPacketCount;
			//flow_sta[i + 1].start_time = start_time;


			//if (global_flowcell_mode || part_flowcell_mode || change_flow)
			//	maxPacketCount_global = maxPacketCount*k_m; //记录需要分析的数据包的数量，以便传到qbb-net-device文件中

			char line[1024] = { 0 };
			std::string a = "";
			std::string b = "";
			std::string c = "";
			std::string d = "";
			std::string e = "";
			infile.getline(line, sizeof(line));
			std::stringstream word(line);
			word >> a;
			word >> b;
			word >> c;
			word >> d;
			word >> e;
			src = std::stoi(b);
			//dst = 18 + 9;
			dst = std::stoi(c);
			double x = std::stod(d);
			start_time = x;
			//maxPacketCount = 1000;
			maxPacketCount = std::stoi(e);
			double cur = rand() % 100 / double(100);
			//std::cout << cur << "\n";
			/*if (cur > 0.1) {
			//dst = std::stoi(c);
			}
			else {
			dst = 18 + 9;
			}*/
			//outfile << i + 1 << " " << src << " " << dst << " " << start_time << " " << maxPacketCount << "\n";
			//std::cout << a << " " << b << " " << c << " " << d << " " << e << "\n";
			std::cout << "flow " << i + 1 << " src " << src << " dst " << dst <<
				" start " << start_time << " size " << maxPacketCount << " \n ";
			//outfile << "size " << maxPacketCount << " size km " << maxPacketCount_global << ",";
			//if (i + 1 >= 6098) {
			//	std::cout << "111111111\n";
			//}
			NS_ASSERT(n.Get(src)->GetNodeType() == 0 && n.Get(dst)->GetNodeType() == 0);
			Ptr<Ipv4> ipv4 = n.Get(dst)->GetObject<Ipv4>();
			Ipv4Address serverAddress = ipv4->GetAddress(1, 0).GetLocal(); //GetAddress(0,0) is the loopback 127.0.0.1

			if (send_in_chunks)/*是否切片*/
			{
				UdpEchoServerHelper server0(port, pg); //Add Priority /*pg是 priority group*/
				ApplicationContainer apps0s = server0.Install(n.Get(dst));
				apps0s.Start(Seconds(app_start_time));
				apps0s.Stop(Seconds(app_stop_time));
				UdpEchoClientHelper client0(serverAddress, port, pg); //Add Priority
				client0.SetAttribute("MaxPackets", UintegerValue(maxPacketCount));
				client0.SetAttribute("Interval", TimeValue(interPacketInterval));
				client0.SetAttribute("PacketSize", UintegerValue(packetSize));
				ApplicationContainer apps0c = client0.Install(n.Get(src));
				apps0c.Start(Seconds(start_time));
				apps0c.Stop(Seconds(stop_time));
			}
			else
			{
				UdpServerHelper server0(port);
				ApplicationContainer apps0s = server0.Install(n.Get(dst));
				apps0s.Start(Seconds(app_start_time));
				apps0s.Stop(Seconds(app_stop_time));
				UdpClientHelper client0(serverAddress, port, pg); //Add Priority

				client0.SetAttribute("MaxPackets", UintegerValue(maxPacketCount_global));
				client0.SetAttribute("Interval", TimeValue(interPacketInterval));
				client0.SetAttribute("PacketSize", UintegerValue(packetSize));

				//client0.SetAttribute("ChunkSize", UintegerValue(l2_chunk_size));//added by lkx
				//client0.SetAttribute("SendTwice", UintegerValue(0));//added by lkx
				//client0.SetAttribute("FlowId", UintegerValue(i + 1));

				ApplicationContainer apps0c = client0.Install(n.Get(src));
				apps0c.Start(Seconds(start_time));
				apps0c.Stop(Seconds(stop_time));
			}
		}
		infile.close();
		//outfile.close();
		//outfile << std::endl;
	}
	//workload_pattern: 0: permutation(possion); 1:random(possion)


	for (uint32_t i = 0; i < tcp_flow_num; i++)
	{
		uint32_t src, dst, pg, maxPacketCount, port;
		double start_time, stop_time;
		while (used_port[port = int(UniformVariable(0, 1).GetValue() * 40000)])
			continue;
		used_port[port] = true;
		tcpflowf >> src >> dst >> pg >> maxPacketCount >> start_time >> stop_time;
		NS_ASSERT(n.Get(src)->GetNodeType() == 0 && n.Get(dst)->GetNodeType() == 0);
		Ptr<Ipv4> ipv4 = n.Get(dst)->GetObject<Ipv4>();
		Ipv4Address serverAddress = ipv4->GetAddress(1, 0).GetLocal();

		Address sinkLocalAddress(InetSocketAddress(Ipv4Address::GetAny(), port));
		PacketSinkHelper sinkHelper("ns3::TcpSocketFactory", sinkLocalAddress);

		ApplicationContainer sinkApp = sinkHelper.Install(n.Get(dst));
		sinkApp.Start(Seconds(app_start_time));
		sinkApp.Stop(Seconds(app_stop_time));

		BulkSendHelper source("ns3::TcpSocketFactory", InetSocketAddress(serverAddress, port));
		// Set the amount of data to send in bytes.  Zero is unlimited.
		source.SetAttribute("MaxBytes", UintegerValue(0));
		ApplicationContainer sourceApps = source.Install(n.Get(src));
		sourceApps.Start(Seconds(start_time));
		sourceApps.Stop(Seconds(stop_time));
	}


	topof.close();
	flowf.close();
	tracef.close();
	tcpflowf.close();

	//
	// Now, do the actual simulation.
	//
	std::cout << "Running Simulation.\n";
	fflush(stdout);
	NS_LOG_INFO("Run Simulation.");//calcute the throughput
								   //Simulator::Schedule(Seconds(2.1), &Run_thread);
	FlowMonitorHelper flowmon;
	Ptr<FlowMonitor> monitor = flowmon.InstallAll();
	//ThroughputMonitor(&flowmon, monitor);
	//Simulator::Schedule(Seconds(2.000), &Record_switch_length);
	//Simulator::Schedule(Seconds(0.1), &ThroughputMonitor, &flowmon, monitor);
	Simulator::Schedule(Seconds(2.000), &Run_thread);
	/*monitor->CheckForLostPackets();

	Ptr<Ipv4FlowClassifier> classifier = DynamicCast<Ipv4FlowClassifier>(flowmon.GetClassifier());

	std::map<FlowId, FlowMonitor::FlowStats> stats = monitor->GetFlowStats();
	for (std::map<FlowId, FlowMonitor::FlowStats>::const_iterator i = stats.begin(); i != stats.end(); ++i)
	{
	Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow(i->first);

	std::cout << "Flow " << i->first << " (" << t.sourceAddress << " -> " << t.destinationAddress << ")\n";
	std::cout << "  Tx Bytes:   " << i->second.txBytes << "\n";
	std::cout << "  Rx Bytes:   " << i->second.rxBytes << "\n";
	std::cout << "  Throughput: " << i->second.rxBytes * 8.0 / (i->second.timeLastRxPacket.GetSeconds() - i->second.timeFirstTxPacket.GetSeconds()) / 1024 / 1024 << " Mbps\n";
	}*/
	Simulator::Stop(MilliSeconds(simulator_stop_time * 1000));
	Simulator::Run();
	//ThroughputMonitor(&flowmon, monitor);

	Simulator::Destroy();
	NS_LOG_INFO("Done.");
	endt = clock();
	std::cout << (double)(endt - begint) / CLOCKS_PER_SEC << "\n";

}
