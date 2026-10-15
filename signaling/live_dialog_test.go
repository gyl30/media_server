package main

import (
	"context"
	"net"
	"reflect"
	"testing"

	"github.com/emiago/sipgo"
	"github.com/emiago/sipgo/sip"
)

// 只截获公开 WriteRequest 的最终请求；不创建网络事务或伪造私有 inviteTx。
type liveDialogRequestCapture struct {
	request *sip.Request
}

func (c *liveDialogRequestCapture) Request(_ context.Context, request *sip.Request) (sip.ClientTransaction, error) {
	c.request = request.Clone()
	return nil, nil
}

func TestLiveDialogRequestMatchesExistingRouting(t *testing.T) {
	ua, err := sipgo.NewUA()
	if err != nil {
		t.Fatal(err)
	}
	defer ua.Close()
	client, err := sipgo.NewClient(ua, sipgo.WithClientAddr("127.0.0.1:5060"))
	if err != nil {
		t.Fatal(err)
	}
	capture := &liveDialogRequestCapture{}
	client.TxRequester = capture
	for _, scenario := range []struct {
		name        string
		contact     bool
		recordRoute []string
	}{
		{name: "rewrite_contact", contact: true},
		{name: "loose_routes", contact: true, recordRoute: []string{"<sip:proxy-one.example:5061;lr>", "<sip:proxy-two.example:5062;lr>"}},
		{name: "strict_route", contact: true, recordRoute: []string{"<sip:proxy-one.example:5061;lr>", "<sip:strict.example:5063>"}},
		{name: "contact_fallback"},
	} {
		t.Run(scenario.name, func(t *testing.T) {
			invite := sip.NewRequest(sip.INVITE, sip.Uri{Scheme: "sip", User: "34020000001320000002", Host: "192.0.2.10", Port: 5060})
			fromParams := sip.NewParams()
			fromParams.Add("tag", "original-local-tag")
			invite.AppendHeader(&sip.FromHeader{Address: sip.Uri{Scheme: "sip", User: "34020000002000000001", Host: "3402000000"}, Params: fromParams})
			invite.AppendHeader(&sip.ToHeader{Address: invite.Recipient})
			callID := sip.CallIDHeader("live-dialog-equivalence")
			invite.AppendHeader(&callID)
			invite.AppendHeader(&sip.CSeqHeader{SeqNo: 123, MethodName: sip.INVITE})
			invite.AppendHeader(&sip.ContactHeader{Address: sip.Uri{Scheme: "sip", User: "34020000002000000001", Host: "192.0.2.20", Port: 5060}})
			invite.AppendHeader(sip.NewHeader("Route", "<sip:original-outbound.example:5070;lr>"))
			invite.SetBody([]byte("original INVITE payload"))
			invite.SetTransport("UDP")
			invite.SetSource("192.0.2.20:5060")
			invite.SetDestination("192.0.2.10:5060")
			invite.Laddr = sip.Addr{IP: net.ParseIP("192.0.2.20"), Port: 5060, Hostname: "192.0.2.20"}
			answer := sip.NewResponseFromRequest(invite, 200, "OK", []byte("original answer payload"))
			answer.To().Params.Add("tag", "final-remote-tag")
			answer.SetSource("198.51.100.30:5080")
			if scenario.contact {
				answer.AppendHeader(&sip.ContactHeader{Address: sip.Uri{Scheme: "sip", User: "remote-channel", Host: "192.0.2.99", Port: 5090}})
			}
			for _, route := range scenario.recordRoute {
				answer.AppendHeader(sip.NewHeader("Record-Route", route))
			}
			inviteText, answerText := invite.String(), answer.String()
			for _, method := range []sip.RequestMethod{sip.ACK, sip.BYE} {
				t.Run(string(method), func(t *testing.T) {
					// 与旧公开 buildReq 路由/CSeq 的等价对照，不调用 Ack/Bye/ReadBye。
					target := invite.Recipient
					if contact := answer.Contact(); contact != nil {
						target = contact.Address
					}
					previous := sip.NewRequest(method, *target.Clone())
					previous.SetTransport(invite.Transport())
					previous.SetSource(invite.Source())
					if method == sip.ACK {
						previous.Laddr = invite.Laddr
					}
					dialog := &sipgo.DialogClientSession{
						Dialog: sipgo.Dialog{InviteRequest: invite.Clone(), InviteResponse: answer.Clone()},
						UA:     &sipgo.DialogUA{Client: client, ContactHDR: *invite.Contact(), RewriteContact: true},
					}
					dialog.Init()
					if err := dialog.WriteRequest(previous); err != nil {
						t.Fatal(err)
					}
					want := capture.request
					// BYE 的序号由会话 owner 分配；普通实时没有 INFO 时首个 BYE 为 INVITE+1。
					build := func() *sip.Request {
						if method == sip.BYE {
							return (&liveSession{invite: invite, answer: answer}).nextDialogRequest(sip.BYE)
						}
						return newLiveDialogRequest(invite, answer, method)
					}
					got := build()
					if err := client.WriteRequest(got); err != nil {
						t.Fatal(err)
					}
					got = capture.request
					if got.Method != want.Method || got.Recipient.String() != want.Recipient.String() || got.Destination() != want.Destination() || got.Source() != want.Source() || got.Transport() != want.Transport() || !reflect.DeepEqual(got.Laddr, want.Laddr) {
						t.Errorf("request routing differs:\ngot=%s destination=%s source=%s local=%v\nwant=%s destination=%s source=%s local=%v", got.StartLine(), got.Destination(), got.Source(), got.Laddr, want.StartLine(), want.Destination(), want.Source(), want.Laddr)
					}
					// Via 的 branch 是每次发送新生成的，不应跨独立请求比较。
					for _, name := range []string{"From", "To", "Call-ID", "CSeq", "Contact", "Max-Forwards", "Content-Length", "Route"} {
						gotHeaders, wantHeaders := got.GetHeaders(name), want.GetHeaders(name)
						if len(gotHeaders) != len(wantHeaders) {
							t.Errorf("%s count=%d want=%d", name, len(gotHeaders), len(wantHeaders))
							continue
						}
						for i := range wantHeaders {
							if gotHeaders[i].Value() != wantHeaders[i].Value() {
								t.Errorf("%s[%d]=%s want=%s", name, i, gotHeaders[i].Value(), wantHeaders[i].Value())
							}
						}
					}
					if len(got.Body()) != 0 {
						t.Error("dialog request copied INVITE or answer payload")
					}
					// 每次独立输出，且不能把输入对象当作可修改的头字段缓存。
					first := build()
					first.From().Params.Add("local-mutation", "1")
					first.To().Params.Add("remote-mutation", "1")
					first.CSeq().SeqNo = 999
					first.Contact().Address.User = "changed"
					first.Recipient.User = "changed-target"
					first.AppendHeader(sip.NewHeader("Route", "<sip:unrelated.example;lr>"))
					second := build()
					if len(second.GetHeaders("Route")) != len(scenario.recordRoute) || second.CSeq().SeqNo != want.CSeq().SeqNo || second.From().Value() != want.From().Value() || second.To().Value() != want.To().Value() || second.Contact().Value() != want.Contact().Value() || second.Recipient.String() != want.Recipient.String() {
						t.Error("independent construction inherited another output's mutations")
					}
					if invite.String() != inviteText || answer.String() != answerText || invite.Source() != "192.0.2.20:5060" || answer.Source() != "198.51.100.30:5080" {
						t.Error("dialog request construction changed input INVITE or answer")
					}
				})
			}
		})
	}
}
