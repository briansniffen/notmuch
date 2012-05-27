#!/usr/bin/env python

import web
from notmuch import Database,Query,Message
import urllib
from datetime import datetime
from mailbox import MaildirMessage
import os
import mimetypes
import email

cachedir = "static" # special for webpy server; changeable if using your own

urls = (
  '/', 'index',
  '/search/(.*)', 'search',
  '/show/(.*)', 'show',
  )

db = Database()

class index:
  def GET(self):
    prefix = '<html><head><title>Notmuch mail</title></head><body><ul>'
    suffix = '</ul></body></html>'
    tags = db.get_all_tags()
    middle = ''
    for tag in tags:
        middle = middle + '<li><a href="/search/tag:%s">%s</a></li>' % (urllib.quote_plus(tag),tag)
    return prefix + middle + suffix

class search:
  def GET(self,terms):
    web.header('Content-type', 'text/html')
    web.header('Transfer-Encoding', 'chunked')
    q = Query(db,terms)
    q.set_sort(Query.SORT.NEWEST_FIRST)
    ts = q.search_threads()
    yield '<html><head><title>Notmuch mail: search results</title></head><body>'
    yield '<h1>%s</h1>' % terms
    for t in ts:
      subj = t.get_subject()
      auths = t.get_authors()
      start,end = t.get_oldest_date(), t.get_newest_date()
      if end-start < (60*60*24):
        time = datetime.fromtimestamp(start).strftime('%Y %b %d %H:%M')
      else:
        start = datetime.fromtimestamp(start).strftime("%Y %b %d")
        end = datetime.fromtimestamp(end).strftime("%Y %b %d")
	time = "%s through %s" % (start,end)
      msgs = t.get_toplevel_messages()
      yield '<h2>%s</h2><p><i>%s</i></p><p><b>%s</b></p>' % (subj,auths,time) # FIXME escaping
      yield show_msgs(msgs)
    yield '</body></html>'

def mailto_addrs(frm):
    frm = email.utils.getaddresses([frm])
    return ','.join(['<a href="mailto:%s">%s</a> ' % ((l,p) if p else (l,l)) for (p,l) in frm])

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
    subj = m.get_header('Subject')
    yield '<html><head><title>Brian\'s mail: %s</title></head><body>' % subj
    headers = ['Subject', 'Date']
    addr_headers = ['To', 'Cc', 'From']
    # FIXME add reply-all link with email.urils.getaddresses
    # FIXME add forward link using mailto with body parameter?
    for header in headers:
      yield '<p><b>%s:</b>%s</p>' % (header,m.get_header(header))
    for header in addr_headers:
      yield '<p><b>%s:</b>%s</p>' % (header, mailto_addrs(m.get_header(header)))
    yield '<hr>'
    msg = MaildirMessage(open(m.get_filename()))
    counter = 0
    for part in mywalk(msg):
      if part=='close-div': yield '</div>'
      elif part.get_content_maintype() == 'multipart': 
        yield '<div class="multipart-%s">' % part.get_content_subtype()
      elif part.get_content_maintype() == 'text':
        if part.get_content_subtype() == 'plain':
          yield '<pre>'
	  yield part.get_payload(decode=True)
	  yield '</pre>'
        elif part.get_content_subtype() == 'html':
          yield part.get_payload(decode=True)
        else:
          filename = link_to_cached_file(part,mid,counter)
	  counter += 1
	  yield '<a href="%s">%s (%s)</a>' % (os.path.join('/static',mid,filename),filename,part.get_content_type())
      elif part.get_content_maintype() == 'image':
        filename = link_to_cached_file(part,mid,counter)
        counter += 1
        yield '<img src="%s" alt="%s">' % (os.path.join('/static',mid,filename),filename)
      else:
        filename = link_to_cached_file(part,mid,counter)
        counter += 1
        yield '<a href="%s">%s (%s)</a>' % (os.path.join('/static',mid,filename),filename,part.get_content_type())
    yield '</body></html>'

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
    fp = open(os.path.join(cachedir, mid, filename), 'wb') # FIXME escape mid,filename
    fp.write(part.get_payload(decode=True))
    fp.close()
    return filename

if __name__ == '__main__': 
    app = web.application(urls, globals())
    app.run()
