package main

import (
	"fmt"
	"io"
	"net"

	"golang.org/x/net/ipv4"
)

type udpBatchSender struct {
	connection     *ipv4.PacketConn
	messages       []ipv4.Message
	headers        [][12]byte
	buffers        [][2][]byte
	sessionIndices []int
	lengths        []int
	loss           packetLoss
}

type packetLoss struct {
	percent int
	seed    uint64
}

func newUDPBatchSender(socket *net.UDPConn, batchSize int, loss packetLoss) *udpBatchSender {
	sender := &udpBatchSender{
		connection:     ipv4.NewPacketConn(socket),
		messages:       make([]ipv4.Message, batchSize),
		headers:        make([][12]byte, batchSize),
		buffers:        make([][2][]byte, batchSize),
		sessionIndices: make([]int, batchSize),
		lengths:        make([]int, batchSize),
		loss:           loss,
	}
	for index := range batchSize {
		sender.buffers[index][0] = sender.headers[index][:]
		sender.messages[index].Buffers = sender.buffers[index][:]
	}
	return sender
}

func (s *udpBatchSender) send(table *sessionTable, sessions []int, unit mediaUnit) (int, int, int, error) {
	packets := 0
	bytesSent := 0
	dropped := 0
	prepared := 0
	for _, sessionIndex := range sessions {
		slot := &table.slots[sessionIndex]
		for _, fragment := range unit.fragments {
			if s.loss.drop(sessionIndex, slot.rtp.sequence) {
				slot.rtp.sequence++
				dropped++
				continue
			}
			slot.rtp.writeHeader(s.headers[prepared][:], slot.payloadType, unit.timestamp, fragment.marker)
			s.buffers[prepared][1] = fragment.payload
			s.messages[prepared].Addr = slot.address
			s.sessionIndices[prepared] = sessionIndex
			s.lengths[prepared] = len(s.headers[prepared]) + len(fragment.payload)
			prepared++
			if prepared != len(s.messages) {
				continue
			}
			sent, bytes, lost, err := s.flush(table, prepared)
			packets += sent
			bytesSent += bytes
			dropped += lost
			prepared = 0
			if err != nil {
				return packets, bytesSent, dropped, err
			}
		}
	}
	if prepared == 0 {
		return packets, bytesSent, dropped, nil
	}
	sent, bytes, lost, err := s.flush(table, prepared)
	return packets + sent, bytesSent + bytes, dropped + lost, err
}

func (l packetLoss) drop(sessionIndex int, sequence uint16) bool {
	if l.percent == 0 {
		return false
	}
	if l.percent == 100 {
		return true
	}
	value := l.seed ^ uint64(sessionIndex)*0x9e3779b97f4a7c15 ^ uint64(sequence)
	value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9
	value = (value ^ (value >> 27)) * 0x94d049bb133111eb
	value ^= value >> 31
	return value%100 < uint64(l.percent)
}

func (s *udpBatchSender) flush(table *sessionTable, count int) (int, int, int, error) {
	sent, err := s.connection.WriteBatch(s.messages[:count], 0)
	bytesSent := 0
	for index := range sent {
		bytesSent += s.lengths[index]
	}
	for index := sent; index < count; index++ {
		table.slots[s.sessionIndices[index]].rtp.sequence--
	}
	if err != nil {
		return sent, bytesSent, count - sent, fmt.Errorf("send RTP batch: %w", err)
	}
	if sent != count {
		return sent, bytesSent, count - sent, io.ErrShortWrite
	}
	return sent, bytesSent, 0, nil
}
