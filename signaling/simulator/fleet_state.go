package main

import (
	"fmt"
	"strconv"
)

const identitySuffixLength = 7

type identitySet struct {
	devicePrefix  string
	channelPrefix string
	deviceStart   int
	channelStart  int
	count         int
}

func newIdentitySet(deviceID, channelID string, count int) (identitySet, error) {
	if !validDigits(deviceID, 20) || !validDigits(channelID, 20) || count <= 0 {
		return identitySet{}, fmt.Errorf("invalid identity range")
	}
	deviceStart, err := strconv.Atoi(deviceID[len(deviceID)-identitySuffixLength:])
	if err != nil {
		return identitySet{}, err
	}
	channelStart, err := strconv.Atoi(channelID[len(channelID)-identitySuffixLength:])
	if err != nil {
		return identitySet{}, err
	}
	if deviceStart+count > 10_000_000 || channelStart+count > 10_000_000 {
		return identitySet{}, fmt.Errorf("identity range exceeds seven-digit suffix")
	}
	return identitySet{
		devicePrefix:  deviceID[:len(deviceID)-identitySuffixLength],
		channelPrefix: channelID[:len(channelID)-identitySuffixLength],
		deviceStart:   deviceStart,
		channelStart:  channelStart,
		count:         count,
	}, nil
}

func (s identitySet) deviceID(index int) string {
	return formatIdentity(s.devicePrefix, s.deviceStart+index)
}

func (s identitySet) channelID(index int) string {
	return formatIdentity(s.channelPrefix, s.channelStart+index)
}

func (s identitySet) deviceIndex(deviceID string) (int, bool) {
	return s.parseIndex(deviceID, s.devicePrefix, s.deviceStart)
}

func (s identitySet) channelIndex(channelID string) (int, bool) {
	return s.parseIndex(channelID, s.channelPrefix, s.channelStart)
}

func (s identitySet) parseIndex(identity, prefix string, start int) (int, bool) {
	if len(identity) != len(prefix)+identitySuffixLength || identity[:len(prefix)] != prefix {
		return 0, false
	}
	value, err := strconv.Atoi(identity[len(prefix):])
	index := value - start
	return index, err == nil && index >= 0 && index < s.count
}

func formatIdentity(prefix string, suffix int) string {
	value := strconv.Itoa(suffix)
	identity := make([]byte, len(prefix)+identitySuffixLength)
	copy(identity, prefix)
	for index := len(prefix); index < len(identity)-len(value); index++ {
		identity[index] = '0'
	}
	copy(identity[len(identity)-len(value):], value)
	return string(identity)
}

func scheduleBucket(destination []int, total, period, bucket int) []int {
	for index := bucket; index < total; index += period {
		destination = append(destination, index)
	}
	return destination
}

func assignMedia(index, workers, phases int) (int, int) {
	return index % workers, (index / workers) % phases
}
