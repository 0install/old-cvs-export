<?xml version="1.0" encoding="UTF-8"?>
<xsl:stylesheet xmlns="http://www.w3.org/1999/xhtml"
		xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
		xmlns:zi="http://zero-install.sourceforge.net/2004/injector/interface"
		version="1.0">

  <xsl:output method="xml" encoding="utf-8"
	doctype-system="http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd"
	doctype-public="-//W3C//DTD XHTML 1.0 Strict//EN"/>

  <xsl:template match="/zi:interface">
    <html>
      <head>
        <link rel='stylesheet' type='text/css' href='style.css' />
        <title>
          <xsl:value-of select="zi:name"/>
        </title>
      </head>
      <body>
        <h1><xsl:value-of select="zi:name"/>Zero Install feed</h1>

	<dl>
	  <dt>Full name</dt><dd><a href='{@uri}'><xsl:value-of select="@uri"/></a></dd>
	  <dt>Summary</dt><dd><xsl:value-of select="zi:summary"/></dd>
	  <dt>Description</dt><dd><xsl:value-of select="zi:description"/></dd>
	</dl>
      </body>
    </html>
  </xsl:template>

</xsl:stylesheet>
