HTML = index.html compare.html faq.html install.html technical.html support.html docs.html security.html packagers.html

all: ${HTML}

%.html: %.xml to_html.xsl
	xsltproc -o $@ --stringparam file "$@" to_html.xsl "$<"
	xmllint --noout --valid $@
