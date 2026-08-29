package main

import (
	"crypto/rand"
	"crypto/subtle"
	"encoding/hex"
	"sync"
	"time"

	"github.com/icholy/digest"
)

type digestNonce struct {
	deviceID string
	expires  time.Time
}

type digestAuthenticator struct {
	mu       sync.Mutex
	realm    string
	password string
	now      func() time.Time
	nonces   map[string]digestNonce
}

func newDigestAuthenticator(realm, password string) *digestAuthenticator {
	return &digestAuthenticator{
		realm:    realm,
		password: password,
		now:      time.Now,
		nonces:   make(map[string]digestNonce),
	}
}

func (a *digestAuthenticator) challenge(deviceID string) (digest.Challenge, error) {
	random := make([]byte, 16)
	if _, err := rand.Read(random); err != nil {
		return digest.Challenge{}, err
	}
	nonce := hex.EncodeToString(random)
	now := a.now()
	a.mu.Lock()
	for value, entry := range a.nonces {
		if !now.Before(entry.expires) {
			delete(a.nonces, value)
		}
	}
	a.nonces[nonce] = digestNonce{deviceID: deviceID, expires: now.Add(5 * time.Minute)}
	a.mu.Unlock()
	return digest.Challenge{
		Realm:     a.realm,
		Nonce:     nonce,
		Opaque:    "media-server-signaling",
		Algorithm: "MD5",
	}, nil
}

func (a *digestAuthenticator) verify(deviceID, method, requestURI, authorization string) bool {
	credentials, err := digest.ParseCredentials(authorization)
	if err != nil || credentials.Username != deviceID || credentials.Realm != a.realm || credentials.URI != requestURI {
		return false
	}

	expected, err := digest.Digest(&digest.Challenge{
		Realm:     a.realm,
		Nonce:     credentials.Nonce,
		Opaque:    "media-server-signaling",
		Algorithm: "MD5",
	}, digest.Options{
		Method:   method,
		URI:      requestURI,
		Username: deviceID,
		Password: a.password,
	})
	if err != nil {
		return false
	}

	a.mu.Lock()
	defer a.mu.Unlock()
	entry, ok := a.nonces[credentials.Nonce]
	if !ok || entry.deviceID != deviceID || !a.now().Before(entry.expires) {
		if ok && !a.now().Before(entry.expires) {
			delete(a.nonces, credentials.Nonce)
		}
		return false
	}
	if subtle.ConstantTimeCompare([]byte(credentials.Response), []byte(expected.Response)) != 1 {
		return false
	}
	delete(a.nonces, credentials.Nonce)
	return true
}
