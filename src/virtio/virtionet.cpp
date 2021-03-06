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

#define PRINT_INFO
#define DEBUG // Allow debuging
#define DEBUG2

#include <virtio/virtionet.hpp>
#include <net/packet.hpp>
#include <kernel/irq_manager.hpp>
#include <kernel/syscalls.hpp>
#include <hw/pci.hpp>
#include <stdio.h>
#include <malloc.h>
#include <string.h>

using namespace net;
constexpr VirtioNet::virtio_net_hdr VirtioNet::empty_header;

const char* VirtioNet::name(){ return "VirtioNet Driver"; }
const net::Ethernet::addr& VirtioNet::mac(){ return _conf.mac; }

void VirtioNet::get_config(){
  Virtio::get_config(&_conf,_config_length);
};

static void drop(Packet_ptr UNUSED(pckt)){
  debug("<VirtioNet->link-layer> No delegate. DROP!\n");
}

VirtioNet::VirtioNet(hw::PCI_Device& d)
  : Virtio(d),
    /** RX que is 0, TX Queue is 1 - Virtio Std. §5.1.2  */
    rx_q(queue_size(0),0,iobase()),  tx_q(queue_size(1),1,iobase()),
    ctrl_q(queue_size(2),2,iobase()),
    _link_out(drop)
{

  INFO("VirtioNet", "Driver initializing");

  uint32_t needed_features = 0
    | (1 << VIRTIO_NET_F_MAC)
    | (1 << VIRTIO_NET_F_STATUS);
  //| (1 << VIRTIO_NET_F_MRG_RXBUF); //Merge RX Buffers (Everything i 1 buffer)
  uint32_t wanted_features = needed_features; /*;
                                                | (1 << VIRTIO_NET_F_CSUM)
                                                | (1 << VIRTIO_F_ANY_LAYOUT)
                                                | (1 << VIRTIO_NET_F_CTRL_VQ)
                                                | (1 << VIRTIO_NET_F_GUEST_ANNOUNCE)
                                                | (1 << VIRTIO_NET_F_CTRL_MAC_ADDR);*/

  negotiate_features(wanted_features);


  CHECK ((features() & needed_features) == needed_features,
         "Negotiated needed features");

  CHECK ((features() & wanted_features) == wanted_features,
         "Negotiated wanted features");

  CHECK(features() & (1 << VIRTIO_NET_F_CSUM),
        "Device handles packets w. partial checksum");

  CHECK(features() & (1 << VIRTIO_NET_F_GUEST_CSUM),
        "Guest handles packets w. partial checksum");

  CHECK(features() & (1 << VIRTIO_NET_F_CTRL_VQ),
        "There's a control queue");

  CHECK(features() & (1 << VIRTIO_F_ANY_LAYOUT),
        "Queue can handle any header/data layout");

  CHECK(features() & (1 << VIRTIO_F_RING_INDIRECT_DESC),
        "We can use indirect descriptors");

  CHECK(features() & (1 << VIRTIO_F_RING_EVENT_IDX),
        "There's a Ring Event Index to use");

  CHECK(features() & (1 << VIRTIO_NET_F_MQ),
        "There are multiple queue pairs");

  if (features() & (1 << VIRTIO_NET_F_MQ))
    printf("\t\t* max_virtqueue_pairs: 0x%x \n",_conf.max_virtq_pairs);

  CHECK(features() & (1 << VIRTIO_NET_F_MRG_RXBUF),
        "Merge RX buffers");


  // Step 1 - Initialize RX/TX queues
  auto success = assign_queue(0, (uint32_t)rx_q.queue_desc());
  CHECK(success, "RX queue assigned (0x%x) to device",
        (uint32_t)rx_q.queue_desc());

  success = assign_queue(1, (uint32_t)tx_q.queue_desc());
  CHECK(success, "TX queue assigned (0x%x) to device",
        (uint32_t)tx_q.queue_desc());

  // Step 2 - Initialize Ctrl-queue if it exists
  if (features() & (1 << VIRTIO_NET_F_CTRL_VQ)) {
    success = assign_queue(2, (uint32_t)tx_q.queue_desc());
    CHECK(success, "CTRL queue assigned (0x%x) to device",
          (uint32_t)ctrl_q.queue_desc());
  }

  // Step 3 - Fill receive queue with buffers
  // DEBUG: Disable
  INFO("VirtioNet", "Adding %i receive buffers of size %i",
       rx_q.size() / 2, bufsize());

  for (int i = 0; i < rx_q.size() / 2; i++) add_receive_buffer();

  // Step 4 - If there are many queues, we should negotiate the number.
  // Set config length, based on whether there are multiple queues
  if (features() & (1 << VIRTIO_NET_F_MQ))
    _config_length = sizeof(config);
  else
    _config_length = sizeof(config) - sizeof(uint16_t);
  // @todo: Specify how many queues we're going to use.

  // Step 5 - get the mac address (we're demanding this feature)
  // Step 6 - get the status - demanding this as well.
  // Getting the MAC + status
  get_config();

  CHECK(_conf.mac.major > 0, "Valid Mac address: %s",
        _conf.mac.str().c_str());


  // Step 7 - 9 - GSO: @todo Not using GSO features yet.

  // Signal setup complete.
  setup_complete((features() & needed_features) == needed_features);
  CHECK((features() & needed_features) == needed_features, "Signalled driver OK");

  // Hook up IRQ handler
  auto del(delegate<void()>::from<VirtioNet,&VirtioNet::irq_handler>(this));
  IRQ_manager::subscribe(irq(),del);
  IRQ_manager::enable_irq(irq());

  // Done
  INFO("VirtioNet", "Driver initialization complete");
  CHECK(_conf.status & 1, "Link up\n");
  rx_q.kick();


};


int VirtioNet::add_receive_buffer(){


  // Virtio Std. § 5.1.6.3
  auto buf = bufstore_.get_raw_buffer();

  debug2("<VirtioNet> Added receive-bufer @ 0x%x \n", (uint32_t)buf);

  Token token1 {
    {buf, sizeof(virtio_net_hdr)},
      Token::IN };

  Token token2 {
    {buf + sizeof(virtio_net_hdr),  (Token::size_type) (bufsize() - sizeof(virtio_net_hdr))},
      Token::IN };

  std::array<Token, 2> tokens {{ token1, token2 }};

  rx_q.enqueue(tokens);

  return 0;
}



void VirtioNet::irq_handler(){

  debug2("<VirtioNet> handling IRQ \n");

  //Virtio Std. § 4.1.5.5, steps 1-3

  // Step 1. read ISR
  unsigned char isr = hw::inp(iobase() + VIRTIO_PCI_ISR);

  // Step 2. A) - one of the queues have changed
  if (isr & 1){

    // This now means service RX & TX interchangeably
    // We need a zipper-solution; we can't receive n packets before sending
    // anything - that's unfair.
    service_queues();
  }

  // Step 2. B)
  if (isr & 2){
    debug("\t <VirtioNet> Configuration change:\n");

    // Getting the MAC + status
    debug("\t             Old status: 0x%x\n",_conf.status);
    get_config();
    debug("\t             New status: 0x%x \n",_conf.status);
  }
  IRQ_manager::eoi(irq());

}

void VirtioNet::service_queues(){
  debug2("<RX Queue> %i new packets \n",
         rx_q.new_incoming());

  /** For RX, we dequeue, add new buffers and let receiver is responsible for
      memory management (they know when they're done with the packet.) */

  int dequeued_rx = 0;
  uint32_t len = 0;
  uint8_t* data;
  int dequeued_tx = 0;

  rx_q.disable_interrupts();
  tx_q.disable_interrupts();
  // A zipper, alternating between sending and receiving
  while(rx_q.new_incoming() or tx_q.new_incoming()){

    // Do one RX-packet
    if (rx_q.new_incoming() ){

      auto res = rx_q.dequeue(); //BUG # 102? + sizeof(virtio_net_hdr);

      data = (uint8_t*) res.data();
      len += res.size();

      auto pckt_ptr = std::make_shared<Packet>
        (data + sizeof(virtio_net_hdr), // Offset buffer (bufstore knows the offseto)
         bufsize()-sizeof(virtio_net_hdr), // Capacity
         res.size() - sizeof(virtio_net_hdr), release_buffer); // Size

      _link_out(pckt_ptr);

      // Requeue a new buffer
      add_receive_buffer();

      dequeued_rx++;

    }

    // Do one TX-packet
    if (tx_q.new_incoming()){
      debug2("<VirtioNet> Dequeing TX");
      tx_q.dequeue();
      dequeued_tx++;
    }

  }

  debug2("<VirtioNet> Service loop about to kick RX if %i \n",
         dequeued_rx);
  // Let virtio know we have increased receive capacity
  if (dequeued_rx)
    rx_q.kick();


  rx_q.enable_interrupts();
  tx_q.enable_interrupts();

  // If we have a transmit queue, eat from it, otherwise let the stack know we
  // have increased transmit capacity
  if (dequeued_tx) {

    debug("<VirtioNet>%i dequeued, transmitting backlog\n", dequeued_tx);

    // transmit as much as possible from the buffer
    if (transmit_queue_){
      auto buf = transmit_queue_;
      transmit_queue_ = 0;
      transmit(buf);
    }else{
      debug("<VirtioNet> Transmit queue is empty \n");
    }

    // If we now emptied the buffer, offer packets to stack
    if (!transmit_queue_ && tx_q.num_free() > 1)
      transmit_queue_available_event_(tx_q.num_free() / 2);
    else
      debug("<VirtioNet> No event: !transmit q %i, num_avail %i \n",
            !transmit_queue_, tx_q.num_free());
  }

  debug("<VirtioNet> Done servicing queues\n");
}

void VirtioNet::add_to_tx_buffer(net::Packet_ptr pckt){
  if (transmit_queue_)
    transmit_queue_->chain(pckt);
  else
    transmit_queue_ = pckt;

#ifdef DEBUG
  size_t chain_length = 1;
  Packet_ptr next = transmit_queue_->tail();
  while (next) {
    chain_length++;
    next = next->tail();
  }
#endif

  debug("Buffering, %i packets chained \n", chain_length);

}

void VirtioNet::transmit(net::Packet_ptr pckt){
  debug2("<VirtioNet> Enqueuing %ib of data. \n",pckt->size());


  /** @note We have to send a virtio header first, then the packet.

      From Virtio std. §5.1.6.6:
      "When using legacy interfaces, transitional drivers which have not
      negotiated VIRTIO_F_ANY_LAYOUT MUST use a single descriptor for the struct
      virtio_net_hdr on both transmit and receive, with the network data in the
      following descriptors."

      VirtualBox *does not* accept ANY_LAYOUT, while Qemu does, so this is to
      support VirtualBox
  */

  int transmitted = 0;
  net::Packet_ptr tail {pckt};

  // Transmit all we can directly
  while (tx_q.num_free() and tail) {
    debug("%i tokens left in TX queue \n", tx_q.num_free());
    on_exit_to_physical_(tail);
    enqueue(tail);
    tail = tail->detach_tail();
    transmitted++;
    if (! tail)
      break;

  }

  // Notify virtio about new packets
  if (transmitted) {
    tx_q.kick();
  }

  // Buffer the rest
  if (tail) {
    add_to_tx_buffer(tail);

    debug("Buffering remaining packets \n");
  }

}

void VirtioNet::enqueue(net::Packet_ptr pckt){


  // This setup requires all tokens to be pre-chained like in SanOS
  Token token1 {{(uint8_t*) &empty_header, sizeof(virtio_net_hdr)},
      Token::OUT };

  Token token2 { {pckt->buffer(), (Token::size_type) pckt->size() }, Token::OUT };

  std::array<Token, 2> tokens {{ token1, token2 }};

  // Enqueue scatterlist, 2 pieces readable, 0 writable.
  tx_q.enqueue(tokens);

}
