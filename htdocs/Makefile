HTML = index.html compare.html faq.html install.html technical.html support.html docs.html security.html packagers.html packagers-cs-1.html

all: ${HTML}

%.html: %.xml to_html.xsl structure.xml
	xsltproc -o $@ --stringparam file "$@" to_html.xsl "$<"
	xmllint --noout --valid $@
