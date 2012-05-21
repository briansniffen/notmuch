#!/usr/bin/env python

import web
from notmuch import Database,Query,Message
import urllib

urls = (
  '/', 'index',
  '/search/(.*)', 'search',
  )

db = Database()

class index:
  def GET(self):
    prefix = '<html><head><title>Brian\'s mail</title></head><body><ul>'
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
    yield '<html><head><title>Brian\'s mail: search results</title></head><body>'
    yield '<h1>%s</h1>' % terms
    for t in ts:
      subj = t.get_subject()
      auths = t.get_authors()
      start,end = t.get_oldest_date(), t.get_newest_date()
      msgs = t.get_toplevel_messages()
      yield '<h2>%s</h2>' % subj # FIXME escaping
      yield '<p><i>%s</i></p>' % auths
      yield '<p><b>%s&ndash;%s</b></p>' % (start,end)
      yield show_msgs(msgs)
    yield '</body></html>'

def show_msgs(msgs):
  r = '<ul>'
  for msg in msgs:
    red = 'black'
    flag = msg.get_flag(Message.FLAG.MATCH)
    if flag: red='red'
    frm = msg.get_header('From')
    subj = msg.get_header('Subject')
    rs = show_msgs(msg.get_replies())
    r += '<li><font color=%s>%s&mdash;%s</font> %s</li>' % (red,frm,subj,rs)
  r += '</ul>'
  return r
    

if __name__ == '__main__': 
    app = web.application(urls, globals())
    app.run()
