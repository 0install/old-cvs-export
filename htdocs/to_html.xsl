<?xml version="1.0"?>

<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                version='1.0'
		xmlns='http://www.w3.org/1999/xhtml'>

  <xsl:output method="xml" encoding="utf-8"
	doctype-system="http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd"
	doctype-public="-//W3C//DTD XHTML 1.0 Strict//EN"/>

  <xsl:param name="file">unknown</xsl:param>

  <xsl:template name='page'>
    <xsl:param name="href">unknown</xsl:param>
    <xsl:choose>
      <xsl:when test='$file = $href'>
        <a class='selected' href="{$href}"><xsl:value-of select='$label'/></a>
      </xsl:when>
      <xsl:otherwise>
        <a href="{$href}"><xsl:value-of select='$label'/></a>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <xsl:template name='make-links'>
    <p class='pages'>
      <xsl:call-template name='page'>
        <xsl:with-param name='href'>index.html</xsl:with-param>
        <xsl:with-param name='label'>Overview</xsl:with-param>
      </xsl:call-template>
      <xsl:call-template name='page'>
        <xsl:with-param name='href'>install.html</xsl:with-param>
        <xsl:with-param name='label'>Installation</xsl:with-param>
      </xsl:call-template>
      <xsl:call-template name='page'>
        <xsl:with-param name='href'>compare.html</xsl:with-param>
        <xsl:with-param name='label'>Motivation</xsl:with-param>
      </xsl:call-template>
      <xsl:call-template name='page'>
        <xsl:with-param name='href'>faq.html</xsl:with-param>
        <xsl:with-param name='label'>FAQ</xsl:with-param>
      </xsl:call-template>
      <xsl:call-template name='page'>
        <xsl:with-param name='href'>technical.html</xsl:with-param>
        <xsl:with-param name='label'>Technical</xsl:with-param>
      </xsl:call-template>
    </p>
  </xsl:template>

  <xsl:template match='/*'>
    <html xml:lang='en' lang='en'>
      <head>
        <link rel='stylesheet' type='text/css' href='style.css' />
        <title>The Zero Install system</title>
      </head>

      <body>
        <div class='heading' style='border-bottom: 2px dashed black'>
          <h1>The Zero Install system</h1>
          <p class='author'>Thomas Leonard &lt;<a href="mailto:tal197@users.sourceforge.net">tal197@users.sourceforge.net</a>&gt;</p>
	  <xsl:call-template name='make-links'/>
        </div>
        <div class='main'>
          <xsl:apply-templates/>
        </div>
	
        <div class='heading' style='border-top: 2px dashed black'>
	  <xsl:call-template name='make-links'/>

	<p>
	  <a href="http://sourceforge.net/projects/zero-install">
	    <img width="88" height="31" alt="SF logo"
     	     src="http://sourceforge.net/sflogo.php?group_id=7023&amp;type=1" />
	  </a>
	  <xsl:text> </xsl:text>
	  <a class='outside' href="http://validator.w3.org/check/referer">
	    <img src="http://www.w3.org/Icons/valid-xhtml10"
	    	 alt="Valid XHTML 1.0!" height="31" width="88" />
	  </a>
	</p>

	</div>
      </body>
    </html>
  </xsl:template>
  
  <xsl:template match='@*|node()'>
    <xsl:copy>
      <xsl:apply-templates select='@*|node()'/>
    </xsl:copy>
  </xsl:template>

</xsl:stylesheet>
