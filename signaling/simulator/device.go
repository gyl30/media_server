package main

import "encoding/xml"

type catalogQuery struct {
	XMLName  xml.Name `xml:"Query"`
	CmdType  string   `xml:"CmdType"`
	SN       int      `xml:"SN"`
	DeviceID string   `xml:"DeviceID"`
}

type catalogResponse struct {
	XMLName    xml.Name `xml:"Response"`
	CmdType    string   `xml:"CmdType"`
	SN         int      `xml:"SN"`
	DeviceID   string   `xml:"DeviceID"`
	SumNum     int      `xml:"SumNum"`
	DeviceList struct {
		Num   int              `xml:"Num,attr"`
		Items []catalogChannel `xml:"Item"`
	} `xml:"DeviceList"`
}

type catalogChannel struct {
	DeviceID string `xml:"DeviceID"`
	Name     string `xml:"Name"`
	ParentID string `xml:"ParentID"`
	Status   string `xml:"Status"`
}

type keepaliveNotify struct {
	XMLName  xml.Name `xml:"Notify"`
	CmdType  string   `xml:"CmdType"`
	SN       int      `xml:"SN"`
	DeviceID string   `xml:"DeviceID"`
	Status   string   `xml:"Status"`
}
