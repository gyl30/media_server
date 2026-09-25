#!/usr/bin/env python3
"""从源码副本构建 GB UDP 阶段延迟采样程序，不修改生产源码。

使用当前 Unix Makefiles RelWithDebInfo 构建目录；每次只测一个源，运行少于 60 秒。
运行生成的 media_server 时，将 OUTPUT_TRACE_DIR 指向已存在的目录。
"""
from pathlib import Path
import argparse
import shlex
import subprocess
import concurrent.futures
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('build')
parser.add_argument('work')
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
build = Path(args.build).resolve()
work = Path(args.work).resolve()
work.mkdir(parents=True, exist_ok=False)
header=work/'trace.h'
header.write_text(r'''
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <unistd.h>
namespace output_trace {
inline std::int64_t now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
inline const auto origin = now();
using times = std::array<std::int64_t, 7>;
inline std::array<std::array<std::atomic<std::int64_t>,3>,240000> frames{};
template<class F> std::size_t slot(const F& f) { return static_cast<std::size_t>((f.pts_ns / 1000000 % 60000 + 60000) % 60000) * 4 + f.track % 4; }
template<class F> void stage(const F& f, std::size_t s) { frames[slot(f)][s].store(now(),std::memory_order_release); }
struct samples {
 times current{};
 std::int64_t callback{};
 std::unordered_map<std::uint64_t,times> pending;
 std::vector<times> done;
 ~samples() {
  const auto* directory=std::getenv("OUTPUT_TRACE_DIR");
  if (!directory || done.empty()) return;
  std::ofstream file(std::string(directory)+"/thread-"+std::to_string(gettid())+".json");
  file << "{\"origin\":" << origin << ",\"pending\":" << pending.size() << ",\"samples\":[";
  bool first=true;
  for(const auto& t:done) { if (!first) file << ','; first=false; file << '[';
   for(std::size_t i=0;i<t.size();++i) { if(i) file << ','; file << t[i]; } file << ']'; }
  file << "]}";
 }
};
inline thread_local samples local;
template<class F> void begin(const F& f) {
 for(std::size_t i=0;i<3;++i) local.current[i]=frames[slot(f)][i].load(std::memory_order_acquire);
 local.current[3]=local.callback;
}
inline std::uint64_t key(const void* p) {
 const auto* b=static_cast<const std::uint8_t*>(p);
 std::uint64_t k=0;
 for(int i=8;i<12;++i) k=(k<<8)|b[i];
 return (k<<16)|(static_cast<std::uint64_t>(b[2])<<8)|b[3];
}
inline void packet(const void* p) {
 const auto k=key(p); if(k%64!=0) return;
 auto t=local.current; t[4]=now(); local.pending[k]=t;
}
inline void submit(const void* p) { const auto k=key(p); if(k%64!=0) return; auto it=local.pending.find(k); if(it!=local.pending.end()) it->second[5]=now(); }
inline void complete(const void* p) { const auto k=key(p); if(k%64!=0) return; auto it=local.pending.find(k); if(it!=local.pending.end()) { it->second[6]=now(); local.done.push_back(it->second); local.pending.erase(it); } }
}
''')
replacements={
'media/core/media_stream.cpp': [('void media_stream::publish(media_frame frame)\n{','void media_stream::publish(media_frame frame)\n{\n    output_trace::stage(frame,0);')],
'media/ps/mpeg_ps_output.cpp': [('void mpeg_ps_output::on_frame(const media_frame& frame)\n{','void mpeg_ps_output::on_frame(const media_frame& frame)\n{\n    output_trace::stage(frame,1);'),('    waiting_for_key_frame_ = false;\n    output_->publish','    waiting_for_key_frame_ = false;\n    output_trace::stage(frame,2);\n    output_->publish')],
'media/gb28181/gb28181_rtp_sender.cpp': [('void gb28181_rtp_sender::on_read(media_read_batch_t<mpeg_ps_frame> batch)\n{','void gb28181_rtp_sender::on_read(media_read_batch_t<mpeg_ps_frame> batch)\n{\n    output_trace::local.callback=output_trace::now();'),('        const auto media_timestamp = entry.frame.media_timestamp;','        output_trace::begin(entry.frame);\n        const auto media_timestamp = entry.frame.media_timestamp;'),('    self.packet_handler_(std::vector<std::uint8_t>(begin, begin + bytes));','    auto packet=std::vector<std::uint8_t>(begin, begin + bytes);\n    output_trace::packet(packet.data());\n    self.packet_handler_(std::move(packet));')],
'media/gb28181/gb28181_udp_sender_session.cpp': [('        rtp_transport_.write(std::span<const std::uint8_t>{data->data(), data->size()}, remote_rtp_endpoint_, yield, error);','        output_trace::submit(data->data());\n        rtp_transport_.write(std::span<const std::uint8_t>{data->data(), data->size()}, remote_rtp_endpoint_, yield, error);\n        output_trace::complete(data->data());')],
}
flags={}
for line in (build/'CMakeFiles/media_core.dir/flags.make').read_text().splitlines():
 if line.startswith('CXX_'):
  name,value=line.split('=',1);flags[name.strip()]=shlex.split(value)
objects=[]
commands=[]
for path,edits in replacements.items():
 text=(root/path).read_text()
 for old,new in edits:
  assert text.count(old)==1,(path,old)
  text=text.replace(old,new)
 source=work/Path(path).name;source.write_text(text)
 obj=source.with_suffix('.o');objects.append(str(obj))
 commands.append(['/usr/bin/c++',*flags['CXX_DEFINES'],*flags['CXX_INCLUDES'],*flags['CXX_FLAGS'],'-include',str(header),'-c',str(source),'-o',str(obj)])
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
 for result in pool.map(lambda c:subprocess.run(c,check=True),commands): pass
link=shlex.split((build/'CMakeFiles/media_server.dir/link.txt').read_text())
link[link.index('-o')+1]=str(work/'media_server')
index=link.index('libmedia_core.a');link[index:index]=objects
subprocess.run(link,cwd=build,check=True)
print(work/'media_server')
