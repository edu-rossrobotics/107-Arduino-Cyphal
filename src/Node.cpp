/**
 * This software is distributed under the terms of the MIT License.
 * Copyright (c) 2020 LXRobotics.
 * Author: Alexander Entinger <alexander.entinger@lxrobotics.com>
 * Contributors: https://github.com/107-systems/107-Arduino-Cyphal/graphs/contributors.
 */

/**************************************************************************************
 * INCLUDE
 **************************************************************************************/

#include "Node.h"

/**************************************************************************************
 * CTOR/DTOR
 **************************************************************************************/

Node::Node(CanFrameTransmitFunc transmit_func,
           CanardNodeID const node_id,
           size_t const tx_queue_capacity,
           size_t const mtu_bytes)
: _canard_hdl{canardInit(Node::o1heap_allocate, Node::o1heap_free)}
, _canard_tx_queue{canardTxInit(tx_queue_capacity, mtu_bytes)}
, _canard_rx_queue{DEFAULT_RX_QUEUE_SIZE}
, _transmit_func{transmit_func}
{
  _canard_hdl.node_id = node_id;
  _canard_hdl.user_reference = reinterpret_cast<void *>(&_o1heap_hdl);
}

/**************************************************************************************
 * PUBLIC MEMBER FUNCTIONS
 **************************************************************************************/

void Node::setNodeId(CanardNodeID const node_id)
{
  _canard_hdl.node_id = node_id;
}

CanardNodeID Node::getNodeId() const
{
  return _canard_hdl.node_id;
}

#include <Arduino.h>

void Node::spin()
{
  handle_receive();
  handle_transmit();
}

void Node::onCanFrameReceived(CanardFrame const & frame, CanardMicrosecond const & rx_timestamp_us)
{
  uint32_t const extended_can_id = frame.extended_can_id;
  size_t   const payload_size    = frame.payload_size;

  std::array<uint8_t, 8> payload;
  payload.fill(0);
  memcpy(payload.data(), frame.payload, std::min(payload_size, payload.size()));

  _canard_rx_queue.enqueue(std::make_tuple(extended_can_id, payload_size, payload, rx_timestamp_us));
}

/**************************************************************************************
 * PRIVATE MEMBER FUNCTIONS
 **************************************************************************************/

void * Node::o1heap_allocate(CanardInstance * const ins, size_t const amount)
{
  O1HeapLibcanard * o1heap = reinterpret_cast<O1HeapLibcanard*>(ins->user_reference);
  return o1heap->allocate(amount);
}

void Node::o1heap_free(CanardInstance * const ins, void * const pointer)
{
  O1HeapLibcanard * o1heap = reinterpret_cast<O1HeapLibcanard*>(ins->user_reference);
  o1heap->free(pointer);
}

void Node::handle_receive()
{
  while (!_canard_rx_queue.isEmpty())
  {
    auto [extended_can_id, payload_size, payload, rx_timestamp_us] = _canard_rx_queue.dequeue();

    CanardFrame frame;
    frame.extended_can_id = extended_can_id;
    frame.payload_size = payload_size;
    frame.payload = reinterpret_cast<const void *>(payload.data());

    receiveOne(frame, micros());
  }
}

void Node::receiveOne(CanardFrame const & frame, CanardMicrosecond const & rx_timestamp_us)
{
  Serial.println(frame.extended_can_id, HEX);

  CanardRxTransfer transfer;
  int8_t const result = canardRxAccept(&_canard_hdl,
                                      rx_timestamp_us,
                                      &frame,
                                      0, /* redundant_transport_index */
                                      &transfer,
                                      nullptr);

  if(result == 1)
  {
    if (_rx_transfer_map.count(transfer.metadata.port_id) > 0)
    {
      OnTransferReceivedFunc transfer_received_func = _rx_transfer_map[transfer.metadata.port_id].transfer_complete_callback;

      if (transfer.metadata.transfer_kind == CanardTransferKindResponse) {
        if ((_tx_transfer_map.count(transfer.metadata.port_id) > 0) && (_tx_transfer_map[transfer.metadata.port_id] == transfer.metadata.transfer_id))
        {
          transfer_received_func(transfer, *this);
          unsubscribe(CanardTransferKindResponse, transfer.metadata.port_id);
        }
      }
      else
        transfer_received_func(transfer, *this);
    }
    /* Free dynamically allocated memory after processing. */
    _canard_hdl.memory_free(&_canard_hdl, transfer.payload);
  }
}

void Node::handle_transmit()
{
  while (transmitOne()) { }
}

bool Node::transmitOne()
{
  if (!_transmit_func)
    return false;

  CanardTxQueueItem const * tx_queue_item = canardTxPeek(&_canard_tx_queue);

  if (tx_queue_item == nullptr)
    return false;

  if (!_transmit_func(tx_queue_item->frame))
    return false;

  _canard_hdl.memory_free(&_canard_hdl, canardTxPop(&_canard_tx_queue, tx_queue_item));
  return true;
}

CanardTransferID Node::getNextTransferId(CanardPortID const port_id)
{
  CanardTransferID const next_transfer_id = (_tx_transfer_map.count(port_id) > 0) ? ((_tx_transfer_map[port_id] + 1) % CANARD_TRANSFER_ID_MAX) : 0;
  _tx_transfer_map[port_id] = next_transfer_id;
  return next_transfer_id;
}

bool Node::subscribe(CanardTransferKind const transfer_kind, CanardPortID const port_id, size_t const payload_size_max, OnTransferReceivedFunc func)
{
  _rx_transfer_map[port_id].transfer_complete_callback = func;
  int8_t const result = canardRxSubscribe(&_canard_hdl,
                                          transfer_kind,
                                          port_id,
                                          payload_size_max,
                                          CANARD_DEFAULT_TRANSFER_ID_TIMEOUT_USEC,
                                          &(_rx_transfer_map[port_id].canard_rx_sub));

  bool const success = (result >= 0);
  return success;
}

bool Node::unsubscribe(CanardTransferKind const transfer_kind, CanardPortID const port_id)
{
  int8_t const result = canardRxUnsubscribe(&_canard_hdl,
                                            transfer_kind,
                                            port_id);

  /* Remove CanardRxSubscription instance from internal list since the
   * structure is no longed needed.
   */
  _rx_transfer_map.erase(port_id);

  bool const success = (result >= 0);
  return success;
}

bool Node::enqeueTransfer(CanardNodeID const remote_node_id, CanardTransferKind const transfer_kind, CanardPortID const port_id, size_t const payload_size, void * payload, CanardTransferID const transfer_id)
{
  CanardTransferMetadata const transfer_metadata =
  {
    .priority       = CanardPriorityNominal,
    .transfer_kind  = transfer_kind,
    .port_id        = port_id,
    .remote_node_id = remote_node_id,
    .transfer_id    = transfer_id,
  };

  /* Serialize transfer into a series of CAN frames */
  int32_t result = canardTxPush(&_canard_tx_queue,
                                &_canard_hdl,
                                CANARD_DEFAULT_TRANSFER_ID_TIMEOUT_USEC,
                                &transfer_metadata,
                                payload_size,
                                payload);
  bool const success = (result >= 0);
  return success;
}
