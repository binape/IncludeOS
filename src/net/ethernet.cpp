// This file is a part of the IncludeOS unikernel - www.includeos.org
//
// Copyright 2015 Oslo and Akershus University College of Applied Sciences
// and Alfred Bratterud
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//#define DEBUG // Allow debugging
//#define DEBUG2

#include <os>

#include <common>

#include <net/ethernet.hpp>
#include <net/packet.hpp>
#include <net/util.hpp>

namespace net {

  // uint16_t(0x0000), uint32_t(0x01000000)
  const Ethernet::addr Ethernet::addr::MULTICAST_FRAME {{0,0,0x01,0,0,0}};

  // uint16_t(0xFFFF), uint32_t(0xFFFFFFFF)
  const Ethernet::addr Ethernet::addr::BROADCAST_FRAME {{0xff,0xff,0xff,0xff,0xff,0xff}};

  // uint16_t(0x3333), uint32_t(0x01000000)
  const Ethernet::addr Ethernet::addr::IPv6mcast_01 {{0x33,0x33,0x01,0,0,0}};

  // uint16_t(0x3333), uint32_t(0x02000000)
  const Ethernet::addr Ethernet::addr::IPv6mcast_02 {{0x33,0x33,0x02,0,0,0}};

  static void ignore(Packet_ptr UNUSED(pckt)) noexcept {
    debug("<Ethernet handler> Ignoring data (no real handler)\n");
  }

  Ethernet::Ethernet(addr mac) noexcept
  : mac_(mac),
    ip4_handler_{ignore},
    ip6_handler_{ignore},
    arp_handler_{ignore}
{}

  void Ethernet::transmit(Packet_ptr pckt) {
    header* hdr = reinterpret_cast<header*>(pckt->buffer());

    // Verify ethernet header
    Expects(hdr->dest.major != 0 || hdr->dest.minor !=0);
    Expects(hdr->type != 0);

    // Add source address
    hdr->src = mac_;

    debug2("<Ethernet OUT> Transmitting %i b, from %s -> %s. Type: %i\n",
           pckt->size(), mac_.str().c_str(), hdr->dest.str().c_str(), hdr->type);

    physical_out_(pckt);
  }

  void Ethernet::bottom(Packet_ptr pckt) {
    Expects(pckt->size() > 0);

    header* eth = reinterpret_cast<header*>(pckt->buffer());

    /** Do we pass on ethernet headers? As for now, yes.
        data += sizeof(header);
        len -= sizeof(header);
    */
    debug2("<Ethernet IN> %s => %s , Eth.type: 0x%x ",
           eth->src.str().c_str(), eth->dest.str().c_str(), eth->type);

    switch(eth->type) {
    case ETH_IP4:
      debug2("IPv4 packet\n");
      ip4_handler_(pckt);
      break;

    case ETH_IP6:
      debug2("IPv6 packet\n");
      ip6_handler_(pckt);
      break;

    case ETH_ARP:
      debug2("ARP packet\n");
      arp_handler_(pckt);
      break;

    case ETH_WOL:
      debug2("Wake-on-LAN packet\n");
      break;

    case ETH_VLAN:
      debug("VLAN tagged frame (not yet supported)");
      break;

    default:
      // This might be 802.3 LLC traffic
      if (net::ntohs(eth->type) > 1500) {
        debug("<Ethernet> UNKNOWN ethertype 0x%x\n", ntohs(eth->type));
      }else {
        debug2("IEEE802.3 Length field: 0x%x\n", ntohs(eth->type));
      }
      break;
    }
  }

} // namespace net
