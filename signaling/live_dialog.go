package main

import "github.com/emiago/sipgo/sip"

func newLiveDialogRequest(invite *sip.Request, answer *sip.Response, method sip.RequestMethod) *sip.Request {
	recipient := invite.Recipient.Clone()
	if contact := answer.Contact(); contact != nil {
		recipient = contact.Address.Clone()
	}
	request := sip.NewRequest(method, *recipient)
	request.SipVersion = invite.SipVersion
	request.AppendHeader(sip.HeaderClone(invite.From()))
	if to := answer.To(); to != nil {
		request.AppendHeader(sip.HeaderClone(to))
	}
	request.AppendHeader(sip.HeaderClone(invite.CallID()))
	cseq := *invite.CSeq()
	cseq.MethodName = method
	request.AppendHeader(&cseq)
	maxForwards := sip.MaxForwardsHeader(70)
	request.AppendHeader(&maxForwards)
	if contact := invite.Contact(); contact != nil {
		request.AppendHeader(sip.HeaderClone(contact))
	}
	routes := answer.GetHeaders("Record-Route")
	for i := len(routes) - 1; i >= 0; i-- {
		request.AppendHeader(sip.NewHeader("Route", routes[i].Value()))
	}
	if len(routes) != 0 {
		if route := request.Route(); !route.Address.UriParams.Has("lr") {
			request.Recipient = *route.Address.Clone()
		}
	} else {
		request.SetDestination(answer.Source())
	}
	request.SetTransport(invite.Transport())
	request.SetSource(invite.Source())
	if method == sip.ACK {
		request.Laddr = invite.Laddr
	}
	request.SetBody(nil)
	return request
}

func (session *liveSession) nextDialogRequest(method sip.RequestMethod) *sip.Request {
	if session.cseq == 0 {
		session.cseq = session.invite.CSeq().SeqNo
	}
	session.cseq++
	request := newLiveDialogRequest(session.invite, session.answer, method)
	request.CSeq().SeqNo = session.cseq
	return request
}
