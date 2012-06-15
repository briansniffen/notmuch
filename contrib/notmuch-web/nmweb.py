#!/usr/bin/env python

import web
from notmuch import Database,Query,Message
import urllib
from datetime import datetime
from mailbox import MaildirMessage
import os
import mimetypes
import email
import string
from jinja2 import Environment, FileSystemLoader # FIXME to PackageLoader
from jinja2 import Markup

cachedir = "static/cache" # special for webpy server; changeable if using your own

env = Environment(autoescape=True,
                  loader=FileSystemLoader('templates'))

urls = (
  '/', 'index',
  '/search/(.*)', 'search',
  '/show/(.*)', 'show',
  )

db = Database()

def urlencode_filter(s):
    if type(s) == 'Markup':
        s = s.unescape()
    s = s.encode('utf8')
    s = urllib.quote_plus(s)
    return Markup(s)
env.filters['url'] = urlencode_filter

class index:
  def GET(self):
    web.header('Content-type', 'text/html')
    web.header('Transfer-Encoding', 'chunked')
    template = env.get_template('index.html')
    tags = db.get_all_tags()
    return template.render(tags=tags)

class search:
  def GET(self,terms):
    web.header('Content-type', 'text/html')
    web.header('Transfer-Encoding', 'chunked')
    if web.input(terms=None).terms:
      terms = web.input().terms
    q = Query(db,terms)
    q.set_sort(Query.SORT.NEWEST_FIRST)
    ts = q.search_threads()
    template = env.get_template('search.html')
    return template.generate(terms=terms,ts=ts)

def format_time_range(start,end):
  if end-start < (60*60*24):
    time = datetime.fromtimestamp(start).strftime('%Y %b %d %H:%M')
  else:
    start = datetime.fromtimestamp(start).strftime("%Y %b %d")
    end = datetime.fromtimestamp(end).strftime("%Y %b %d")
    time = "%s through %s" % (start,end)
  return time
env.globals['format_time_range'] = format_time_range

def mailto_addrs(frm):
    frm = email.utils.getaddresses([frm])
    return ','.join(['<a href="mailto:%s">%s</a> ' % ((l,p) if p else (l,l)) for (p,l) in frm])
env.globals['mailto_addrs'] = mailto_addrs

def show_msgs(msgs):
  r = '<ul>'
  for msg in msgs:
    red = 'black'
    flag = msg.get_flag(Message.FLAG.MATCH)
    if flag: red='red'
    frm = msg.get_header('From')
    frm = mailto_addrs(frm)
    subj = msg.get_header('Subject')
    lnk = urllib.quote_plus(msg.get_message_id())
    rs = show_msgs(msg.get_replies())
    r += '<li><font color=%s>%s&mdash;<a href="/show/%s">%s</a></font> %s</li>' % (red,frm,lnk,subj,rs)
  r += '</ul>'
  return r
env.globals['show_msgs'] = show_msgs

# As email.message.walk, but showing close tags as well
def mywalk(self):
    yield self
    if self.is_multipart():
        for subpart in self.get_payload():
            for subsubpart in mywalk(subpart):
                yield subsubpart
        yield 'close-div'

class show:
  def GET(self,mid):
    web.header('Content-type', 'text/html')
    web.header('Transfer-Encoding', 'chunked')
    q = Query(db,'id:'+mid)
    m = list(q.search_messages())[0]
    template = env.get_template('show.html')
    # FIXME add reply-all link with email.urils.getaddresses
    # FIXME add forward link using mailto with body parameter?
    # FIXME handle cid: url scheme
    # FIXME come up with some brilliant plan for script tags and other dangerous things
    return template.render(m=m,mid=mid)

def format_message(fn,mid):
    msg = MaildirMessage(open(fn))
    counter = 0
    for part in mywalk(msg):
      if part=='close-div': yield '</div>'
      elif part.get_content_maintype() == 'multipart': 
        ## FIXME tabbed interface for multipart/alternative
        yield '<div class="multipart-%s">' % part.get_content_subtype()
      elif part.get_content_maintype() == 'text':
        if part.get_content_subtype() == 'plain':
          yield '<pre>'
	  yield part.get_payload(decode=True).decode(part.get_content_charset('ascii'))
	  yield '</pre>'
        elif part.get_content_subtype() == 'html':
          yield replace_cids(part.get_payload(decode=True).decode(part.get_content_charset('ascii')),mid)
        else:
          filename = link_to_cached_file(part,mid,counter)
	  counter += 1
	  yield '<a href="%s">%s (%s)</a>' % (os.path.join('/',cachedir,mid,filename),filename,part.get_content_type())
      elif part.get_content_maintype() == 'image':
        filename = link_to_cached_file(part,mid,counter)
        counter += 1
        yield '<img src="%s" alt="%s">' % (os.path.join('/',cachedir,mid,filename),filename)
      else:
        filename = link_to_cached_file(part,mid,counter)
        counter += 1
        yield '<a href="%s">%s (%s)</a>' % (os.path.join('/',cachedir,mid,filename),filename,part.get_content_type())
env.globals['format_message'] = format_message

def replace_cids(body,mid):
    return string.replace(body,'cid:',("/%s/%s/" % (cachedir,mid)))

def link_to_cached_file(part,mid,counter):
    filename = part.get_filename()
    if not filename:
      ext = mimetypes.guess_extension(part.get_content_type())
      if not ext:
        ext = '.bin'
      filename = 'part-%03d%s' % (counter, ext)
    try:
      os.makedirs(os.path.join(cachedir,mid))
    except OSError:
      pass
    fn = os.path.join(cachedir, mid, filename) # FIXME escape mid,filename
    fp = open(fn, 'wb')
    fp.write(part.get_payload(decode=True))
    fp.close()
    if 'Content-ID' in part:
        cid = part['Content-ID']
        cid = cid[1:-1] # chop <brackets>
        cid_fn = os.path.join(cachedir, mid, cid) # FIXME escape mid,cid
        try:
            os.unlink(cid_fn)
        except OSError:
            pass
        os.link(fn,cid_fn)
    return filename

if __name__ == '__main__': 
    app = web.application(urls, globals())
    app.run()
