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
import re
from jinja2 import Environment, FileSystemLoader # FIXME to PackageLoader
from jinja2 import Markup

prefix = "/btsmail"

webprefix = prefix + "/static"

cachedir = "static/cache" # special for webpy server; changeable if using your own

env = Environment(autoescape=True,
                  loader=FileSystemLoader('templates'))

urls = (
  '/', 'index',
  '/search/(.*)', 'search',
  '/show/(.*)', 'show',
  )

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
    base = env.get_template('base.html')
    template = env.get_template('index.html')
    db = Database()
    tags = db.get_all_tags()
    return template.render(tags=tags,
                           title="Notmuch webmail",
			   prefix=prefix,
                           sprefix=webprefix)

class search:
  def GET(self,terms):
    redir = False
    if web.input(terms=None).terms:
      redir = True
      terms = web.input().terms
    if web.input(afters=None).afters:
      afters = web.input(afters=None).afters[:-3]
    else:
      afters='0'
    if web.input(befores=None).befores:
      befores = web.input(befores=None).befores
    else:
      befores = '4294967296' # 2^32
    if int(afters) > 0 or int(befores) < 4294967296:
      redir = True
      terms += ' %s..%s' % (afters,befores)
    if redir:
      raise web.seeother('/search/%s' % urllib.quote_plus(terms))
    web.header('Content-type', 'text/html')
    web.header('Transfer-Encoding', 'chunked')
    db = Database()
    q = Query(db,terms)
    q.set_sort(Query.SORT.NEWEST_FIRST)
    ts = q.search_threads()
    template = env.get_template('search.html')
    return template.generate(terms=terms,
                             ts=ts,
                             title=terms,
                             prefix=prefix,
			     sprefix=webprefix)

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
    r += '<li><font color=%s>%s&mdash;<a href="%s/show/%s">%s</a></font> %s</li>' % (red,frm,prefix,lnk,subj,rs)
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
    db = Database()
    q = Query(db,'id:'+mid)
    m = list(q.search_messages())[0]
    template = env.get_template('show.html')
    # FIXME add reply-all link with email.urils.getaddresses
    # FIXME add forward link using mailto with body parameter?
    # FIXME come up with some brilliant plan for script tags and other dangerous things
    return template.render(m=m,
                           mid=mid,
                           title=m.get_header('Subject'),
                           prefix=prefix,
			   sprefix=webprefix)

def format_message(fn,mid):
    msg = MaildirMessage(open(fn))
    return format_message_walk(msg,mid)

def decodeAnyway(txt,charset='ascii'):
  try:
    out = txt.decode(charset)
  except UnicodeDecodeError:
    try:
      out = txt.decode('utf-8')
    except UnicodeDecodeError:
      out = txt.decode('latin1')
  return out

def format_message_walk(msg,mid):
    counter = 0
    cid_refd = []
    for part in mywalk(msg):
      if part=='close-div': 
          yield '</div>'
      elif part.get_content_maintype() == 'multipart': 
        yield '<div class="multipart-%s">' % part.get_content_subtype()
        if part.get_content_subtype() == 'alternative':
          yield '<ul>'
          for subpart in part.get_payload():
            yield ('<li><a href="#%s">%s</a></li>' %
                   (string.replace(subpart.get_content_type(),
                                   '/', '-'),
                    subpart.get_content_type()))
          yield '</ul>'
      elif part.get_content_type() == 'message/rfc822':
          # FIXME extract subject, date, to/cc/from into a separate template and use it here
          yield '<div class="message-rfc822">'
      elif part.get_content_maintype() == 'text':
        if part.get_content_subtype() == 'plain':
          yield '<div id="text-plain"><pre>'
          out = part.get_payload(decode=True)
          out = decodeAnyway(out,part.get_content_charset('ascii'))
          yield out
          yield '</pre></div>'
        elif part.get_content_subtype() == 'html':
          yield '<div id="text-html">'
          unb64 = part.get_payload(decode=True)
          decoded = decodeAnyway(unb64,part.get_content_charset('ascii'))
          cid_refd += find_cids(decoded)
          part.set_payload(replace_cids(decoded,mid).encode(part.get_content_charset('ascii')))
	  (filename,cid) = link_to_cached_file(part,mid,counter)
	  counter +=1
	  yield '<iframe class="embedded-html" src="%s">' % os.path.join(prefix,cachedir,mid,filename)
          yield '</div>'
        else:
          yield '<div id="%s">' % string.replace(part.get_content_type(),'/','-')
          (filename,cid) = link_to_cached_file(part,mid,counter)
          counter += 1
          yield '<a href="%s">%s (%s)</a>' % (os.path.join(prefix,
                                                           cachedir,
                                                           mid,
                                                           filename),
                                              filename,
                                              part.get_content_type())
          yield '</div>'
      elif part.get_content_maintype() == 'image':
        (filename,cid) = link_to_cached_file(part,mid,counter)
        if cid not in cid_refd:
          counter += 1
          yield '<img src="%s" alt="%s">' % (os.path.join(prefix,
                                                          cachedir,
                                                          mid,
                                                          filename),
                                             filename)
      else:
        (filename,cid) = link_to_cached_file(part,mid,counter)
        counter += 1
        yield '<a href="%s">%s (%s)</a>' % (os.path.join(prefix,
                                                         cachedir,
                                                         mid,
                                                         filename),
                                            filename,
                                            part.get_content_type())
env.globals['format_message'] = format_message

def replace_cids(body,mid):
    return string.replace(body,'cid:',os.path.join(prefix,cachedir,mid)+'/')

def find_cids(body):
    return re.findall(r'cid:([^ "\'>]*)', body)

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
    if part.get_content_maintype()=='text':
      data = part.get_payload(decode=True)
      data = decodeAnyway(data,part.get_content_charset('ascii')).encode('utf-8')
    else:
      try:
        data = part.get_payload(decode=True)
      except:
        data = part.get_payload(decode=False)
    if data: fp.write(data)
    fp.close()
    if 'Content-ID' in part:
        cid = part['Content-ID']
        if cid[0]=='<' and cid[-1]=='>': cid = cid[1:-1]
        cid_fn = os.path.join(cachedir, mid, cid) # FIXME escape mid,cid
        try:
            os.unlink(cid_fn)
        except OSError:
            pass
        os.link(fn,cid_fn)
        return (filename,cid)
    else:
        return (filename,None)

if __name__ == '__main__': 
    app = web.application(urls, globals())
    app.run()
