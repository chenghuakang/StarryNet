#!/usr/bin/python
# -*- coding: UTF-8 -*-
"""
StarryNet: empowering researchers to evaluate futuristic integrated space and terrestrial networks.
author: Zeqi Lai (zeqilai@tsinghua.edu.cn) and Yangtao Deng (dengyt21@mails.tsinghua.edu.cn)
"""

from starrynet.sn_observer import *
from starrynet.sn_synchronizer import *

if __name__ == "__main__":
    # Starlink 5*5: 25 satellite nodes, 2 ground stations.
    # The node index sequence is: 25 sattelites, 2 ground stations.
    # In this example, 25 satellites and 2 ground stations are one AS.

    GS_lat_long = [[50.110924, 8.682127], [46.635700, 14.311817]
                   ]  # latitude and longitude of frankfurt and  Austria
    configuration_file_path = "./config.json"
    hello_interval = 5  # hello_interval(s) in OSPF. 1-200 are supported.

    print('Start StarryNet.')
    sn = StarryNet(configuration_file_path, GS_lat_long, hello_interval)
    
    sn.create_nodes()
    print(sn.node_mid_dict.keys()) #For test 
    sn.create_links()
  
    node1 = 'SH1O1S1'
    node2 = 'SH1O1S2'
    time_index = 2
    # LLA of a node at a certain time
    LLA = sn.get_position(node1, time_index)
    print(f'\nLatitude, Longitude, Altitude of {node1}: {LLA}')

    # distance between nodes at a certain time
    node_distance = sn.get_distance(node1, node2, time_index)
    print(f'\n{node1}-{node2} distance(km): {node_distance}')

    # neighbor nodes at a certain time
    neighbors = sn.get_neighbors(node1, time_index)
    print(f'\n{node1} neighbors: {neighbors}')
    
    # GS connected to the node at a certain time
    GSes = sn.get_GSes(node1, time_index)
    print(f"\n{node1} GSes: {GSes}")

    time_index = 1
    sn.get_utility(time_index)  # CPU and memory useage

    # IP dict of a node
    IP_dict = sn.get_IP(node1)
    print(f'\n{node1} IP addresses: {IP_dict}')

    # run OSPF daemon on all nodes
    # sn.run_routing_daemon()
    
    # run OSPF daemon on selected nodes
    sn.run_routing_daemon(node_lst=['GS0', 'SH1O2S2', 'SH1O2S3', 'SH1O3S3', 'GS1'])

    sat = 'SH1O1S1'
    des = 'SH1O1S3'
    next_hop = 'SH1O1S2'
    time_index = 2
    # set the next hop at a certain time. Sat and NextHopSat are neighbors.
    sn.set_next_hop(sat, des, next_hop, time_index)

    time_index = 3
    # routing table of a node at a certain time. The output file will be written at the working directory.
    sn.check_routing_table(sat, time_index)

    node1 = 'SH1O1S1'
    node2 = 'SH1O1S2'
    time_index = 4
    # ping msg of two nodes at a certain time. The output file will be written at the working directory.
    sn.set_ping(node1, node2, time_index)
    # perf msg of two nodes at a certain time. The output file will be written at the working directory.
    sn.set_iperf(node1, node2, time_index)


    # run OSPF daemon on all nodes
    # sn.run_routing_daemon()
    
    # run OSPF daemon on selected nodes

    sn.run_routing_daemon(node_lst=sn.get_node_names())
    #sn.run_routing_daemon(node_lst=[
     # 'GS0', 'SH1O25S14', 'SH1O26S14', 'SH1O27S14', 'SH1O27S13', 'GS1'])

    sn.check_routing_table('GS0', 4)

    ratio = 0.3
    time_index = 15
    # random damage of a given ratio at a certain time
    sn.set_damage(ratio, time_index)

    sn.check_routing_table('GS0', 30)

    time_index = 35
    # recover the damages at a certain time
    sn.set_recovery(time_index)

    sn.check_routing_table('GS0', 45)

    sn.start_emulation()

    if input('clear environment?[y/n]').strip().lower()[:1] == 'y':
        sn.clean()
